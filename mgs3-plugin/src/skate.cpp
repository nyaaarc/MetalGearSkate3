#include "skate.hpp"

#include <xinput.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <mutex>
#include <fstream>
#include <optional>
#include <vector>
#include <cmath>
#include <cstring>
#include <numbers>
#include <string>

#include "skate_ffi.h"
#include "game.hpp"
#include "log.hpp"
#include "pose.hpp"

#pragma comment(lib, "xinput.lib")

namespace skate {
namespace {

// MGS3 works in millimetres, the Skate engine in metres.
constexpr float kMmPerUnit = 1000.f;
// Snake's gameplay position sits about this far above the floor (measured in v001a).
constexpr float kSnakeHeightMm = 800.f;
// Half-size of the stand-in floor, in engine units.
constexpr float kFloorHalf = 150.f;
constexpr int kToggleKey = VK_F5;

struct Ffi {
  decltype(&mgsk_session_new) session_new;
  decltype(&mgsk_install_world) install_world;
  decltype(&mgsk_period) period;
  decltype(&mgsk_activate) activate;
  decltype(&mgsk_tick) tick;
  decltype(&mgsk_root) root;
  decltype(&mgsk_last_error) last_error;
  decltype(&mgsk_retarget_init) retarget_init;
  decltype(&mgsk_snake_pose) snake_pose;
  decltype(&mgsk_state) state;
  decltype(&mgsk_rail_count) rail_count;
};

// From skate_ffi.dll when it has the .geom reader: decodes and installs a stage's collision,
// returns the triangle count or -1.
using InstallGeomFn = int(__cdecl*)(MgskSession*, const char*, float);

std::filesystem::path g_plugin_dir;

// Guards ignore Snake while skating: equip the stealth camouflage (the game's own invisibility
// item) and put his previous item back afterwards. `stealth_while_skating=0` in the ini turns it off.
class Stealth {
 public:
  void begin(bool enabled) {
    uint8_t* link = game::linkvar();
    if (!enabled || !link || active_) return;
    memcpy(&previous_, link + game::kLinkCurrentItem, sizeof(previous_));
    const int16_t stealth = game::kItemStealthCamouflage;
    memcpy(link + game::kLinkCurrentItem, &stealth, sizeof(stealth));
    active_ = true;
  }
  // Keep it equipped (the game may switch items on its own).
  void hold() {
    uint8_t* link = game::linkvar();
    if (!active_ || !link) return;
    const int16_t stealth = game::kItemStealthCamouflage;
    memcpy(link + game::kLinkCurrentItem, &stealth, sizeof(stealth));
  }
  void end() {
    uint8_t* link = game::linkvar();
    if (active_ && link) memcpy(link + game::kLinkCurrentItem, &previous_, sizeof(previous_));
    active_ = false;
  }

 private:
  bool active_ = false;
  int16_t previous_ = 0;
};

// -1 none, 0 stop, 1 start: a pending request from the test harness.
std::atomic<int> g_request{-1};
std::mutex g_status_mutex;
Status g_status;

void set_status(const Status& s) {
  std::lock_guard lock(g_status_mutex);
  g_status = s;
}
HMODULE g_ffi_module = nullptr;

// Board pose published by the skate thread for the renderer.
std::mutex g_pose_mutex;
float g_pose[kBoardBodies * 16];
bool g_pose_valid = false;
// Engine chase camera (position in mm, basis columns, vertical fov in degrees), same mutex.
float g_camera[kCameraFloats];
bool g_camera_valid = false;

void publish_pose(const float* pose, const float* camera = nullptr) {
  std::lock_guard lock(g_pose_mutex);
  if (pose) memcpy(g_pose, pose, sizeof(g_pose));
  g_pose_valid = pose != nullptr;
  if (camera) memcpy(g_camera, camera, sizeof(g_camera));
  g_camera_valid = camera != nullptr;
}

template <typename T>
bool bind(T& slot, const char* name) {
  slot = reinterpret_cast<T>(GetProcAddress(g_ffi_module, name));
  if (!slot) log("skate: skate_ffi.dll has no export %s", name);
  return slot != nullptr;
}

bool load_ffi(Ffi& f) {
  return bind(f.session_new, "mgsk_session_new") && bind(f.install_world, "mgsk_install_world") &&
         bind(f.period, "mgsk_period") && bind(f.activate, "mgsk_activate") && bind(f.tick, "mgsk_tick") &&
         bind(f.root, "mgsk_root") && bind(f.last_error, "mgsk_last_error") &&
         bind(f.retarget_init, "mgsk_retarget_init") && bind(f.snake_pose, "mgsk_snake_pose") &&
         bind(f.state, "mgsk_state") && bind(f.rail_count, "mgsk_rail_count");
}

// The stage's collision file, from the first `.geom` entry in its manifest. The manifests sit
// under the language folders (us/ has none), and every language lists the same geom.
std::optional<std::filesystem::path> stage_geom(const std::string& stage) {
  wchar_t exe[MAX_PATH];
  GetModuleFileNameW(nullptr, exe, MAX_PATH);
  const auto game_dir = std::filesystem::path(exe).parent_path();
  for (const char* language : {"fr", "gr", "it", "sp", "jp", "us"}) {
    std::ifstream manifest(game_dir / language / "stage" / stage / "manifest.txt");
    for (std::string line; std::getline(manifest, line);) {
      const auto comma = line.find(',');
      const auto first = line.substr(0, comma);
      if (first.size() > 5 && first.ends_with(".geom")) return game_dir / std::filesystem::path(first);
    }
  }
  return std::nullopt;
}

// Pre-exported collision (`tools/geom.py <geom folder> --bin <dir>`): u32 count, count*9 float32
// in metres. Used until skate_ffi.dll reads .geom itself.
std::vector<float> load_collision_bin(const std::filesystem::path& file) {
  std::ifstream in(file, std::ios::binary);
  uint32_t count = 0;
  if (!in.read(reinterpret_cast<char*>(&count), sizeof(count))) return {};
  std::vector<float> tris(static_cast<size_t>(count) * 9);
  if (!in.read(reinterpret_cast<char*>(tris.data()), static_cast<std::streamsize>(tris.size() * sizeof(float)))) {
    return {};
  }
  return tris;
}

std::string ini_string(const char* key, const char* fallback) {
  char value[MAX_PATH] = {};
  GetPrivateProfileStringA("skate", key, fallback, value, MAX_PATH,
                           (g_plugin_dir / L"MetalGearSkate.ini").string().c_str());
  return value;
}

// MGS3 yaw (radians) <-> engine heading (radians): engine = sign * mgs + offset.
// Tunable from MetalGearSkate.ini while the two conventions are being matched up.
struct HeadingMap {
  float sign = 1.f;
  float offset = 0.f;

  float to_engine(int16_t yaw) const {
    return sign * static_cast<float>(yaw) * (2.f * std::numbers::pi_v<float>) / 65536.f + offset;
  }
  int16_t to_mgs(float heading) const {
    const float mgs = (heading - offset) / sign;
    return static_cast<int16_t>(std::lround(mgs * 65536.f / (2.f * std::numbers::pi_v<float>)));
  }
};

HeadingMap heading_map() {
  HeadingMap h;
  h.sign = std::stof(ini_string("heading_sign", "1"));
  h.offset = std::stof(ini_string("heading_offset_deg", "0")) * std::numbers::pi_v<float> / 180.f;
  return h;
}

// Two triangles: a flat square at height y centred on (x, z), engine units.
std::array<float, 18> flat_floor(float x, float y, float z) {
  const float h = kFloorHalf;
  return {x - h, y, z - h, x + h, y, z + h, x + h, y, z - h,
          x - h, y, z - h, x - h, y, z + h, x + h, y, z + h};
}

// PS2 button bit -> XInput button bit.
struct ButtonMap {
  int ps2, xinput;
};
constexpr ButtonMap kButtonMap[] = {
    {0x4000, XINPUT_GAMEPAD_A},          {0x2000, XINPUT_GAMEPAD_B},
    {0x8000, XINPUT_GAMEPAD_X},          {0x1000, XINPUT_GAMEPAD_Y},
    {0x0400, XINPUT_GAMEPAD_LEFT_SHOULDER}, {0x0800, XINPUT_GAMEPAD_RIGHT_SHOULDER},
    {0x0008, XINPUT_GAMEPAD_START},      {0x0001, XINPUT_GAMEPAD_BACK},
    {0x0002, XINPUT_GAMEPAD_LEFT_THUMB}, {0x0004, XINPUT_GAMEPAD_RIGHT_THUMB},
    {0x0010, XINPUT_GAMEPAD_DPAD_UP},    {0x0040, XINPUT_GAMEPAD_DPAD_DOWN},
    {0x0080, XINPUT_GAMEPAD_DPAD_LEFT},  {0x0020, XINPUT_GAMEPAD_DPAD_RIGHT},
};

// 0..255 with centre 128 -> XInput's signed axis.
int16_t axis(uint8_t value, bool flip) {
  const int v = (static_cast<int>(value) - 128) * 257;
  return static_cast<int16_t>(std::clamp(flip ? -v : v, -32768, 32767));
}

// The game's own pad state (captured by the pad hook before blanking) works for every controller Steam Input supports (the Steam Controller
// isn't visible to XInput while the game uses Steam Input). A real XInput pad takes priority.
MgskControls read_pad() {
  MgskControls c{};
  XINPUT_STATE state{};
  if (XInputGetState(0, &state) == ERROR_SUCCESS) {
    const XINPUT_GAMEPAD& g = state.Gamepad;
    c.buttons = g.wButtons;
    c.left_trigger = g.bLeftTrigger;
    c.right_trigger = g.bRightTrigger;
    c.left[0] = g.sThumbLX;
    c.left[1] = g.sThumbLY;
    c.right[0] = g.sThumbRX;
    c.right[1] = g.sThumbRY;
    return c;
  }
  game::Pad pad;
  if (!game::pad(pad)) return c;
  const game::Pad* p = &pad;
  for (const ButtonMap& b : kButtonMap) {
    if (p->buttons & b.ps2) c.buttons = static_cast<uint16_t>(c.buttons | b.xinput);
  }
  c.left_trigger = (p->buttons & 0x0100) ? 255 : 0;
  c.right_trigger = (p->buttons & 0x0200) ? 255 : 0;
  // PS2 sticks grow downward; XInput's grow upward.
  c.left[0] = axis(p->left_x, false);
  c.left[1] = axis(p->left_y, true);
  c.right[0] = axis(p->right_x, false);
  c.right[1] = axis(p->right_y, true);
  return c;
}

class Clock {
 public:
  explicit Clock(double period) {
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    ticks_per_period_ = static_cast<LONGLONG>(period * static_cast<double>(f.QuadPart));
    QueryPerformanceCounter(&next_);
  }
  // Sleeps until the next period boundary.
  void wait() {
    next_.QuadPart += ticks_per_period_;
    for (;;) {
      LARGE_INTEGER now;
      QueryPerformanceCounter(&now);
      const LONGLONG left = next_.QuadPart - now.QuadPart;
      if (left <= 0) break;
      Sleep(left > ticks_per_period_ / 4 ? 1 : 0);
    }
  }

 private:
  LONGLONG ticks_per_period_ = 0;
  LARGE_INTEGER next_{};
};

DWORD WINAPI run(LPVOID) {
  Ffi f{};
  if (!load_ffi(f)) return 0;
  // `skate_data`: the converter output's `assets` folder.
  const std::string data = ini_string("skate_data", "");
  const HeadingMap headings = heading_map();
  log("skate: heading map sign %.0f offset %.1f deg", headings.sign, headings.offset * 180.f / std::numbers::pi_v<float>);
  if (data.empty()) {
    log("skate: set [skate] skate_data in MetalGearSkate.ini to the converted data's assets folder");
    return 0;
  }

  // Load once up front; creating a session takes seconds.
  const auto initial = flat_floor(0, 0, 0);
  const float origin[3] = {0, 0, 0};
  const DWORD started = GetTickCount();
  MgskSession* session = f.session_new(data.c_str(), initial.data(), 2, origin, 0.f);
  if (!session) {
    log("skate: session failed: %s", f.last_error());
    return 0;
  }
  const auto install_geom = reinterpret_cast<InstallGeomFn>(GetProcAddress(g_ffi_module, "mgsk_install_geom"));
  // `collision`: folder of pre-exported <geom>.bin files, used when the DLL can't read .geom.
  const std::filesystem::path collision_dir = ini_string("collision", "");
  if (!install_geom) log("skate: skate_ffi.dll has no .geom reader; using %s", collision_dir.string().c_str());
  std::string collision_stage;  // stage whose collision the session holds; empty = flat floor
  // Optional: older skate_ffi.dll builds have no board export (then nothing is drawn).
  const auto pose_fn = reinterpret_cast<decltype(&mgsk_board_pose)>(GetProcAddress(g_ffi_module, "mgsk_board_pose"));
  if (!pose_fn) log("skate: skate_ffi.dll has no mgsk_board_pose; the board won't be drawn");
  const auto camera_fn = reinterpret_cast<decltype(&mgsk_camera)>(GetProcAddress(g_ffi_module, "mgsk_camera"));
  if (!camera_fn) log("skate: skate_ffi.dll has no mgsk_camera; the game camera stays");
  // Retargeting onto Snake's own skeleton (his model file in the game's assets).
  bool posing = false;
  {
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    const auto mdl = std::filesystem::path(exe).parent_path() / "assets" / "mdl" / "us" / "sna_def.mdl";
    const int mirror = std::stoi(ini_string("mirror_x", "0"));
    posing = f.retarget_init(session, mdl.string().c_str(), mirror) == 0;
    log(posing ? "skate: retargeting onto %s (mirror_x %d)" : "skate: retargeting unavailable for %s (mirror_x %d): %s",
        mdl.string().c_str(), mirror, posing ? "" : f.last_error());
  }
  float snake_bones[pose::kSnakeBones * 16];
  const bool stealth_enabled = ini_string("stealth_while_skating", "1") != "0";
  Stealth stealth;
  const float period = f.period(session);
  log("skate: engine ready in %lu ms (tick %.4f s). F5 toggles skating.", GetTickCount() - started, period);

  bool skating = false;
  bool key_was_down = false;
  Status st;
  st.ready = true;
  set_status(st);
  int ticks = 0;
  Clock clock(period);
  for (;;) {
    clock.wait();
    const bool key_down = (GetAsyncKeyState(kToggleKey) & 0x8000) != 0;
    const int remote = g_request.exchange(-1);
    const bool toggled = (key_down && !key_was_down) || (remote == 1 && !skating) || (remote == 0 && skating);
    key_was_down = key_down;

    game::Vec3* snake = game::snake_position();
    if (toggled) {
      if (skating) {
        skating = false;
        game::block_input(false);
        pose::publish(nullptr);
      stealth.end();
        log("skate: off");
      } else if (!snake) {
        log("skate: no player to put on a board");
      } else {
        const float x = snake->x / kMmPerUnit, z = snake->z / kMmPerUnit;
        const float y = (snake->y - kSnakeHeightMm) / kMmPerUnit;
        const float at[3] = {x, y, z};
        const int16_t* yaw = game::snake_yaw();
        const float heading = yaw ? headings.to_engine(*yaw) : 0.f;
        // Real stage collision when available (loaded once per stage), else a flat floor at his feet.
        const std::string stage = game::current_stage() ? game::current_stage() : "";
        bool world_ok = !stage.empty() && stage == collision_stage;
        if (!world_ok && !stage.empty()) {
          if (const auto geom = stage_geom(stage)) {
            const DWORD t0 = GetTickCount();
            int count = -1;
            if (install_geom) {
              count = install_geom(session, geom->string().c_str(), 1.f / kMmPerUnit);
            } else if (const auto tris = load_collision_bin(collision_dir / geom->filename().replace_extension(".bin"));
                       !tris.empty() && f.install_world(session, tris.data(), static_cast<uint32_t>(tris.size() / 9)) == 0) {
              count = static_cast<int>(tris.size() / 9);
            }
            if (count >= 0) {
              world_ok = true;
              collision_stage = stage;
              st.collision_triangles = count;
              st.rails = f.rail_count(session);
              log("skate: %s collision from %s: %d triangles in %lu ms", stage.c_str(),
                  geom->filename().string().c_str(), count, GetTickCount() - t0);
            } else {
              log("skate: %s collision failed: %s", stage.c_str(), f.last_error());
            }
          } else {
            log("skate: no .geom listed for stage '%s'", stage.c_str());
          }
        }
        if (!world_ok) {
          const auto floor = flat_floor(x, y, z);
          world_ok = f.install_world(session, floor.data(), 2) == 0;
          collision_stage.clear();
          st.collision_triangles = -1;
        }
        if (!world_ok || f.activate(session, at, heading)) {
          log("skate: start failed: %s", f.last_error());
        } else {
          skating = true;
          game::block_input(true);
          stealth.begin(stealth_enabled);
          ticks = 0;
          log("skate: on at %.1f %.1f %.1f (mm), Snake yaw %d -> heading %.1f deg", snake->x, snake->y, snake->z,
              yaw ? *yaw : 0, heading * 180.f / std::numbers::pi_v<float>);
        }
      }
    }
    st.skating = skating;
    if (!skating) {
      publish_pose(nullptr);
      set_status(st);
      continue;
    }
    if (!snake) {
      skating = false;
      game::block_input(false);
      pose::publish(nullptr);
      stealth.end();
      log("skate: player gone, off");
      continue;
    }

    const MgskControls pad = read_pad();
    float m[16];
    if (f.tick(session, &pad) || f.root(session, m)) {
      log("skate: engine error, off: %s", f.last_error());
      skating = false;
      game::block_input(false);
      pose::publish(nullptr);
      stealth.end();
      continue;
    }
    // Translation is the last column of the column-major root matrix.
    snake->x = m[12] * kMmPerUnit;
    snake->y = m[13] * kMmPerUnit + kSnakeHeightMm;
    snake->z = m[14] * kMmPerUnit;
    // Face Snake along the skater: the root's local +Z axis (third column) is its forward.
    const float heading = std::atan2(m[8], m[10]);
    if (int16_t* yaw = game::snake_yaw()) *yaw = headings.to_mgs(heading);
    if (int16_t* target = game::snake_target_yaw()) *target = headings.to_mgs(heading);
    float pose[kBoardBodies * 16];
    float camera[kCameraFloats];
    const bool have_camera = camera_fn && camera_fn(session, camera) == 0;
    if (have_camera) {
      for (int i = 0; i < 3; ++i) camera[i] *= kMmPerUnit;
    }
    publish_pose(pose_fn && pose_fn(session, pose, kBoardBodies) == kBoardBodies ? pose : nullptr,
                 have_camera ? camera : nullptr);
    {
      const float dx = m[12] - st.skater[0], dz = m[14] - st.skater[2];
      st.speed_mps = std::sqrt(dx * dx + dz * dz) / period;
      if (f.state(session, st.engine_state, sizeof(st.engine_state)) != 0) st.engine_state[0] = 0;
    }
    st.skater[0] = m[12];
    st.skater[1] = m[13];
    st.skater[2] = m[14];
    st.heading_deg = heading * 180.f / std::numbers::pi_v<float>;
    stealth.hold();
    if (posing) pose::publish(f.snake_pose(session, snake_bones, pose::kSnakeBones) == pose::kSnakeBones ? snake_bones : nullptr);
    set_status(st);
    if (++ticks % 120 == 0) {
      const pose::Stats ps = pose::stats();
      log("skate: pose hook calls %llu, palette slots posed %llu in %llu Snake draws, last bone count %d, nearest "
          "57-bone object %.0f mm, relative %d",
          ps.calls, ps.applied, ps.draws, ps.last_count, ps.nearest_mm, ps.relative);
      log("skate: skater at %.2f %.2f %.2f (m) heading %.0f deg; pad buttons %04X left %d,%d right %d,%d", m[12], m[13], m[14],
          heading * 180.f / std::numbers::pi_v<float>, pad.buttons, pad.left[0], pad.left[1], pad.right[0], pad.right[1]);
    }
  }
}

}  // namespace

void request(bool on) { g_request = on ? 1 : 0; }

bool camera(float (&out)[kCameraFloats]) {
  std::lock_guard lock(g_pose_mutex);
  if (g_camera_valid) memcpy(out, g_camera, sizeof(g_camera));
  return g_camera_valid;
}

bool board_pose(float (&out)[kBoardBodies * 16]) {
  std::lock_guard lock(g_pose_mutex);
  if (g_pose_valid) memcpy(out, g_pose, sizeof(g_pose));
  return g_pose_valid;
}

Status status() {
  std::lock_guard lock(g_status_mutex);
  return g_status;
}

void start(const std::filesystem::path& plugin_dir, HMODULE ffi) {
  g_plugin_dir = plugin_dir;
  g_ffi_module = ffi;
  if (HANDLE thread = CreateThread(nullptr, 0, run, nullptr, 0, nullptr)) CloseHandle(thread);
}

}  // namespace skate
