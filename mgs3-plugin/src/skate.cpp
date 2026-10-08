#include "skate.hpp"

#include <xinput.h>

#include <algorithm>
#include <array>
#include <fstream>
#include <optional>
#include <vector>
#include <cmath>
#include <numbers>
#include <string>

#include "skate_ffi.h"
#include "game.hpp"
#include "log.hpp"

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
};

// From skate_ffi.dll when it has the .geom reader: decodes and installs a stage's collision,
// returns the triangle count or -1.
using InstallGeomFn = int(__cdecl*)(MgskSession*, const char*, float);

std::filesystem::path g_plugin_dir;
HMODULE g_ffi_module = nullptr;

template <typename T>
bool bind(T& slot, const char* name) {
  slot = reinterpret_cast<T>(GetProcAddress(g_ffi_module, name));
  if (!slot) log("skate: skate_ffi.dll has no export %s", name);
  return slot != nullptr;
}

bool load_ffi(Ffi& f) {
  return bind(f.session_new, "mgsk_session_new") && bind(f.install_world, "mgsk_install_world") &&
         bind(f.period, "mgsk_period") && bind(f.activate, "mgsk_activate") && bind(f.tick, "mgsk_tick") &&
         bind(f.root, "mgsk_root") && bind(f.last_error, "mgsk_last_error");
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

// The game's own pad state works for every controller Steam Input supports (the Steam Controller
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
  const game::Pad* p = game::pad();
  if (!p) return c;
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
  const float period = f.period(session);
  log("skate: engine ready in %lu ms (tick %.4f s). F5 toggles skating.", GetTickCount() - started, period);

  bool skating = false;
  bool key_was_down = false;
  int ticks = 0;
  Clock clock(period);
  for (;;) {
    clock.wait();
    const bool key_down = (GetAsyncKeyState(kToggleKey) & 0x8000) != 0;
    const bool toggled = key_down && !key_was_down;
    key_was_down = key_down;

    game::Vec3* snake = game::snake_position();
    if (toggled) {
      if (skating) {
        skating = false;
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
        }
        if (!world_ok || f.activate(session, at, heading)) {
          log("skate: start failed: %s", f.last_error());
        } else {
          skating = true;
          ticks = 0;
          log("skate: on at %.1f %.1f %.1f (mm), Snake yaw %d -> heading %.1f deg", snake->x, snake->y, snake->z,
              yaw ? *yaw : 0, heading * 180.f / std::numbers::pi_v<float>);
        }
      }
    }
    if (!skating) continue;
    if (!snake) {
      skating = false;
      log("skate: player gone, off");
      continue;
    }

    const MgskControls pad = read_pad();
    float m[16];
    if (f.tick(session, &pad) || f.root(session, m)) {
      log("skate: engine error, off: %s", f.last_error());
      skating = false;
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
    if (++ticks % 120 == 0) {
      log("skate: skater at %.2f %.2f %.2f (m) heading %.0f deg; pad buttons %04X left %d,%d right %d,%d", m[12], m[13], m[14],
          heading * 180.f / std::numbers::pi_v<float>, pad.buttons, pad.left[0], pad.left[1], pad.right[0], pad.right[1]);
    }
  }
}

}  // namespace

void start(const std::filesystem::path& plugin_dir, HMODULE ffi) {
  g_plugin_dir = plugin_dir;
  g_ffi_module = ffi;
  if (HANDLE thread = CreateThread(nullptr, 0, run, nullptr, 0, nullptr)) CloseHandle(thread);
}

}  // namespace skate
