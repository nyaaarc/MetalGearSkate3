#include "skate.hpp"

#include <xinput.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <fstream>
#include <optional>
#include <vector>
#include <cmath>
#include <cstring>
#include <numbers>
#include <string>
#include <thread>

#include "skate_ffi.h"
#include "audio.hpp"
#include "game.hpp"
#include "log.hpp"
#include "core_api.hpp"
#include "camera.hpp"
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
// A skater this far (engine units) below where he last rode has left the stage's collision.
constexpr float kFallLimit = 3.f;
// Over no floor, or this far (engine units) above the nearest, the skater has left the stage. MGS3
// starts Snake's fall (and kills him) as soon as there's no ground under him, so act at once.
constexpr float kMaxDrop = 4.f;
// The floor is looked for below this far above the root: in a wipeout the root sinks into it, up to
// 1.65 m in the s091a bail recipe (0.5 m step); 1.5 m here turned some of those bails into respawns.
constexpr float kFloorProbe = 2.5f;
// MGS3 runs its own gravity on Snake between our writes wherever *its* collision has no floor
// under him, which can differ from the .geom the skater rides. The game also nudges him a little
// on ordinary floor (it settles him to its own idea of his height), so only a drop this big
// (mm) between two writes counts: by then his fall speed has built up over a fraction of a
// second. Used on skate off: Snake is handed back on the last spot the game held him. (Respawning
// the skater mid-ride on this signal misfired on stairs and slopes, freezing him in place.)
constexpr float kGameDropMm = 150.f;

struct Ffi {
  decltype(&mgsk_session_new) session_new;
  decltype(&mgsk_session_free) session_free;
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
  decltype(&mgsk_bone_count) bone_count;
  decltype(&mgsk_bone_name) bone_name;
  decltype(&mgsk_bones) bones;
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

// -1 none, 0 stop, 1 start, 2 start at g_start_at (even while skating): a pending harness request.
std::atomic<int> g_request{-1};
// -1 none, 0 off, 1 on: a pending stealth change from the test harness.
std::atomic<int> g_stealth{-1};
// Ticks to trace from the next one on (harness `trace <ticks>`), 0 = none pending.
std::atomic<int> g_trace_request{0};
// Optional start pose for the next harness "skate on": mm (Snake's waist) and MGS3 heading in
// degrees. Guarded by g_status_mutex; consumed by the skate thread.
struct StartAt {
  bool set = false;
  float x = 0, y = 0, z = 0, heading_deg = 0;
};
StartAt g_start_at;
std::mutex g_status_mutex;
Status g_status;
Recent g_recent;  // guarded by g_status_mutex
bool g_recent_empty = true;

// From the skate thread, every tick.
void note_recent(const char* state, float y, float speed) {
  std::lock_guard lock(g_status_mutex);
  if (g_recent_empty) {
    g_recent = {};
    g_recent.max_y = g_recent.min_y = y;
    g_recent_empty = false;
  }
  g_recent.max_y = std::max(g_recent.max_y, y);
  g_recent.min_y = std::min(g_recent.min_y, y);
  g_recent.max_speed_mps = std::max(g_recent.max_speed_mps, speed);
  const char* last = strrchr(g_recent.states, ',');
  last = last ? last + 1 : g_recent.states;
  if (state[0] && strcmp(last, state) != 0) {
    const size_t used = strlen(g_recent.states);
    snprintf(g_recent.states + used, sizeof(g_recent.states) - used, "%s%s", used ? "," : "", state);
  }
}

void set_status(const Status& s) {
  std::lock_guard lock(g_status_mutex);
  g_status = s;
}
HMODULE g_ffi_module = nullptr;
core::CoreShared* g_shared = nullptr;
std::atomic<bool> g_stop{false};  // core unload: the skate thread hands over and exits
HANDLE g_thread = nullptr;
// Game frames, counted by frame() on the game's thread; the skate thread ticks on them (Pacer).
std::mutex g_frame_mutex;
std::condition_variable g_frame_cv;
uint64_t g_frames = 0;  // guarded by g_frame_mutex

// Board pose published by the skate thread for the renderer.
std::mutex g_pose_mutex;
float g_pose[kBoardBodies * 16];
bool g_pose_valid = false;
// Engine chase camera (position in mm, basis columns, vertical fov in degrees), same mutex.
float g_camera[kCameraFloats];
bool g_camera_valid = false;

// Snake's retargeted bones from the same tick as g_pose (same mutex), so the renderer can draw
// board and Snake from one tick (reading them separately mid-tick jittered him on the board).
float g_snake[pose::kSnakeBones * 16];
bool g_snake_valid = false;
uint64_t g_pose_tick = 0;  // counts publishes with Snake's bones (same mutex), for diagnostics

void publish_pose(const float* pose, const float* camera = nullptr, const float* snake = nullptr) {
  std::lock_guard lock(g_pose_mutex);
  if (pose) memcpy(g_pose, pose, sizeof(g_pose));
  g_pose_valid = pose != nullptr;
  if (snake) memcpy(g_snake, snake, sizeof(g_snake));
  g_snake_valid = snake != nullptr;
  if (snake) ++g_pose_tick;
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
  return bind(f.session_new, "mgsk_session_new") && bind(f.session_free, "mgsk_session_free") && bind(f.install_world, "mgsk_install_world") &&
         bind(f.period, "mgsk_period") && bind(f.activate, "mgsk_activate") && bind(f.tick, "mgsk_tick") &&
         bind(f.root, "mgsk_root") && bind(f.last_error, "mgsk_last_error") &&
         bind(f.retarget_init, "mgsk_retarget_init") && bind(f.snake_pose, "mgsk_snake_pose") &&
         bind(f.state, "mgsk_state") && bind(f.rail_count, "mgsk_rail_count") &&
         bind(f.bone_count, "mgsk_bone_count") && bind(f.bone_name, "mgsk_bone_name") && bind(f.bones, "mgsk_bones");
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
MgskControls read_pad(char* source = nullptr) {
  MgskControls c{};
  // Harness input first: one injected frame per engine tick (frame-exact, as the recipes assume).
  game::Pad injected;
  const bool have_injected = game::take_injected(injected);
  XINPUT_STATE state{};
  if (source) *source = have_injected ? 'i' : 'g';
  if (!have_injected && XInputGetState(0, &state) == ERROR_SUCCESS) {
    if (source) *source = 'x';
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
  if (!have_injected && !game::pad(pad)) return c;
  const game::Pad* p = have_injected ? &injected : &pad;
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
  // Counts periods from now (no catch-up burst for the time it wasn't used).
  void restart() { QueryPerformanceCounter(&next_); }
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

// Paces the engine: one tick per game frame (frame(), the main camera's update) while the game
// draws, so each frame moves the skater, board and camera exactly one tick on. On the clock alone
// the engine beat against the game's 60 fps: 5-15% of frames got no new tick and the next one
// two, a judder in the whole picture, worse under CPU load (devlog 2026-10-08). The skater now
// keeps the game's time: when the game drops frames he slows with it. With no frame for two
// periods (loading, menus, no camera hook) it ticks on the clock.
class Pacer {
 public:
  explicit Pacer(double period)
      : clock_(period),
        timeout_(std::chrono::duration<double>(period * 2)),
        min_gap_(std::chrono::duration_cast<std::chrono::steady_clock::duration>(
            std::chrono::duration<double>(period / 2))) {
    std::lock_guard lock(g_frame_mutex);
    seen_ = g_frames;
  }
  void wait() {
    bool framed;
    {
      std::unique_lock lock(g_frame_mutex);
      const auto due = [this] { return g_frames != seen_; };
      framed = due() || (!free_ && g_frame_cv.wait_for(lock, timeout_, due));
      // Frames that piled up behind a slow tick get one tick: a pause, not a jump.
      seen_ = g_frames;
    }
    if (!framed) {
      if (!free_) clock_.restart();
      free_ = true;
      clock_.wait();
      return;
    }
    free_ = false;
    // Two updates of the main camera within a frame mustn't run the engine twice as fast.
    std::this_thread::sleep_until(last_ + min_gap_);
    last_ = std::chrono::steady_clock::now();
  }

 private:
  Clock clock_;
  std::chrono::duration<double> timeout_;
  std::chrono::steady_clock::duration min_gap_;
  std::chrono::steady_clock::time_point last_{};
  uint64_t seen_ = 0;
  bool free_ = true;
};

// Off the board (BipedGround, BipedAir) the skater carries it in one hand. Snake takes the
// skater's joint rotations on his own bone lengths, anchored at the ankles, so his hand ends up
// centimetres from the skater's and the board hung beside it, not in it (user, 2026-10-09).
// Carried, the board moves by the gap between the two hands, eased in and out over a few ticks
// so it doesn't jump as he picks it up or puts it down.
class Carry {
 public:
  // The skater's hands, and Snake's bones they drive (retarget.rs MAP: 9 left, 14 right; the
  // other way round when the skater is mirrored).
  void init(const Ffi& f, const MgskSession* session, bool mirror) {
    const int count = f.bone_count(session);
    if (count <= 0) return;
    bones_.assign(static_cast<size_t>(count) * 16, 0.f);
    for (int i = 0; i < count; ++i) {
      char name[64] = {};
      if (f.bone_name(session, static_cast<uint32_t>(i), name, sizeof(name)) < 0) continue;
      if (strcmp(name, "LEFTHAND") == 0) hands_[0] = i;
      if (strcmp(name, "RIGHTHAND") == 0) hands_[1] = i;
    }
    snake_hands_[0] = mirror ? 14 : 9;
    snake_hands_[1] = mirror ? 9 : 14;
    if (hands_[0] < 0 || hands_[1] < 0) log("skate: the skater has no LEFTHAND/RIGHTHAND bones; the carried board isn't moved to Snake's hand");
  }

  // `root`: the skater's root (column-major, metres); `snake`: his bones (MGS3 layout, mm) or
  // null; `board`: the board bodies (column-major, metres), moved in place.
  void apply(const Ffi& f, const MgskSession* session, const char* state, const float* root, const float* snake,
             float* board) {
    constexpr float kStep = 1.f / 8;     // about 0.13 s in or out at 60 Hz
    constexpr float kHeldM = 0.6f;       // a hand this close to the deck holds it
    bool carried = snake && hands_[0] >= 0 && hands_[1] >= 0 && strncmp(state, "Biped", 5) == 0 &&
                   f.bones(session, bones_.data(), static_cast<uint32_t>(bones_.size() / 16)) * 16 ==
                       static_cast<int>(bones_.size());
    if (carried) {
      const float* deck = board + MGSK_BODY_DECK * 16;
      float best = kHeldM * kHeldM;
      int held = -1;
      float hand[2][3];
      for (int h = 0; h < 2; ++h) {
        const float* b = bones_.data() + hands_[h] * 16;
        float d2 = 0;
        for (int r = 0; r < 3; ++r) {
          hand[h][r] = root[r] * b[12] + root[4 + r] * b[13] + root[8 + r] * b[14] + root[12 + r];
          d2 += (hand[h][r] - deck[12 + r]) * (hand[h][r] - deck[12 + r]);
        }
        if (d2 < best) {
          best = d2;
          held = h;
        }
      }
      carried = held >= 0;
      if (carried) {
        const float* s = snake + snake_hands_[held] * 16;
        for (int r = 0; r < 3; ++r) offset_[r] = s[12 + r] / kMmPerUnit - hand[held][r];
        if (weight_ <= 0.f) {
          log("skate: carrying the board in the %s hand; moved %.0f mm onto Snake's", held ? "right" : "left",
              kMmPerUnit * std::sqrt(offset_[0] * offset_[0] + offset_[1] * offset_[1] + offset_[2] * offset_[2]));
        }
      }
    }
    weight_ = std::clamp(weight_ + (carried ? kStep : -kStep), 0.f, 1.f);
    if (weight_ <= 0.f) return;
    for (int body = 0; body < kBoardBodies; ++body) {
      for (int r = 0; r < 3; ++r) board[body * 16 + 12 + r] += weight_ * offset_[r];
    }
  }

 private:
  std::vector<float> bones_;
  int hands_[2] = {-1, -1};
  int snake_hands_[2] = {9, 14};
  float offset_[3] = {};
  float weight_ = 0.f;
};

// A session built ahead on another thread. The engine can leave non-finite state in a session
// (docs/devlog.md, run 8), and a fresh one takes seconds to load, so one is kept ready to swap in.
class Spare {
 public:
  template <class Make>
  void prepare(Make make) {
    if (building_.exchange(true)) return;
    if (worker_.joinable()) worker_.join();  // the last build's thread, already done
    worker_ = std::thread([this, make] {
      ready_ = make();
      building_ = false;
    });
  }
  MgskSession* take() { return ready_.exchange(nullptr); }
  void set(MgskSession* s) { ready_ = s; }
  // Joins the builder: its thread runs this module's code to the end, even after `building_`
  // clears, so the module mustn't be freed before it has exited.
  void wait() {
    if (worker_.joinable()) worker_.join();
  }
  ~Spare() { wait(); }

 private:
  std::atomic<MgskSession*> ready_{nullptr};
  std::atomic<bool> building_{false};
  std::thread worker_;
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
  wchar_t exe[MAX_PATH];
  GetModuleFileNameW(nullptr, exe, MAX_PATH);
  // Retargeting onto Snake's own skeleton (his model file in the game's assets).
  const auto mdl = std::filesystem::path(exe).parent_path() / "assets" / "mdl" / "us" / "sna_def.mdl";
  const int mirror = std::stoi(ini_string("mirror_x", "0"));
  // A session on a flat floor, set up for posing Snake (`posing` says whether that worked).
  auto make_session = [f, data, mdl, mirror](bool* posing) -> MgskSession* {
    const auto initial = flat_floor(0, 0, 0);
    const float origin[3] = {0, 0, 0};
    MgskSession* s = f.session_new(data.c_str(), initial.data(), 2, origin, 0.f);
    if (!s) {
      log("skate: session failed: %s", f.last_error());
      return nullptr;
    }
    const bool ok = f.retarget_init(s, mdl.string().c_str(), mirror) == 0;
    if (!ok) log("skate: retargeting unavailable for %s (mirror_x %d): %s", mdl.string().c_str(), mirror, f.last_error());
    if (posing) *posing = ok;
    return s;
  };
  const DWORD started = GetTickCount();
  bool posing = false;
  // A session handed over by the previous core (a reload) is reused as is.
  MgskSession* session = static_cast<MgskSession*>(g_shared->session);
  if (session) {
    posing = g_shared->posing;
  } else {
    session = make_session(&posing);
  }
  g_shared->session = nullptr;
  if (!session) return 0;
  if (posing) log("skate: retargeting onto %s (mirror_x %d)", mdl.string().c_str(), mirror);
  Carry carry;
  if (posing) carry.init(f, session, mirror != 0);
  // The skater's sound (Skate 3's audio runtime in the DLL, from setup's audio export).
  if (ini_string("audio", "1") != "0" && audio::start(g_ffi_module, std::stof(ini_string("audio_volume", "4")))) {
    audio::use_session(session, data.c_str());
  }
  Spare spare;
  auto prepare_spare = [&spare, make_session] { spare.prepare([make_session] { return make_session(nullptr); }); };
  if (g_shared->spare) {
    spare.set(static_cast<MgskSession*>(g_shared->spare));
    g_shared->spare = nullptr;
  } else {
    prepare_spare();
  }
  const auto install_geom = reinterpret_cast<InstallGeomFn>(GetProcAddress(g_ffi_module, "mgsk_install_geom"));
  // `collision`: folder of pre-exported <geom>.bin files, used when the DLL can't read .geom.
  const std::filesystem::path collision_dir = ini_string("collision", "");
  if (!install_geom) log("skate: skate_ffi.dll has no .geom reader; using %s", collision_dir.string().c_str());
  std::string collision_stage = g_shared->collision_stage;  // stage whose collision the session holds; empty = flat floor
  // Optional: older skate_ffi.dll builds have no board export (then nothing is drawn).
  const auto pose_fn = reinterpret_cast<decltype(&mgsk_board_pose)>(GetProcAddress(g_ffi_module, "mgsk_board_pose"));
  if (!pose_fn) log("skate: skate_ffi.dll has no mgsk_board_pose; the board won't be drawn");
  const auto camera_fn = reinterpret_cast<decltype(&mgsk_camera)>(GetProcAddress(g_ffi_module, "mgsk_camera"));
  if (!camera_fn) log("skate: skate_ffi.dll has no mgsk_camera; the game camera stays");
  const auto floor_fn = reinterpret_cast<decltype(&mgsk_floor_below)>(GetProcAddress(g_ffi_module, "mgsk_floor_below"));
  if (!floor_fn) log("skate: skate_ffi.dll has no mgsk_floor_below; only deep falls are caught");
  float snake_bones[pose::kSnakeBones * 16];
  bool stealth_enabled = ini_string("stealth_while_skating", "0") != "0";
  Stealth stealth;
  const float period = f.period(session);
  log("skate: engine ready in %lu ms (tick %.4f s). F5 toggles skating.", GetTickCount() - started, period);

  bool skating = false;
  bool key_was_down = false;
  Status st;
  st.ready = true;
  st.collision_triangles = g_shared->collision_triangles;
  st.rails = g_shared->rails;
  set_status(st);
  int ticks = 0;
  // Installs the current stage's collision (once per stage, else a flat floor at `at`) and puts
  // the skater down at `at` (engine units) facing `heading`. False with the error logged.
  auto begin = [&](const float at[3], float heading) -> bool {
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
      const auto floor = flat_floor(at[0], at[1], at[2]);
      world_ok = f.install_world(session, floor.data(), 2) == 0;
      collision_stage.clear();
      st.collision_triangles = -1;
    }
    if (!world_ok || f.activate(session, at, heading)) {
      log("skate: start failed: %s", f.last_error());
      return false;
    }
    return true;
  };
  // Swaps the spare in for a session the engine broke. False while no spare is ready yet.
  auto replace_session = [&]() -> bool {
    MgskSession* fresh = spare.take();
    if (!fresh) return false;
    audio::use_session(fresh, data.c_str());
    f.session_free(session);
    session = fresh;
    collision_stage.clear();  // a new session has only the flat floor
    prepare_spare();
    log("skate: swapped in a fresh engine session");
    return true;
  };
  bool broken = false;  // the session failed: replace it before the next start
  // Where the skater last rode on the ground (engine units, heading), for respawns.
  struct Spot {
    float at[3];
    float heading;
  } last_good{}, riding_spot{{}, NAN};
  bool placed = false;  // the skater was just put somewhere: no speed from that jump
  float written_y = NAN;  // Snake's y as we last wrote it (mm)
  int game_falling = 0;   // ticks in a row the game moved Snake down between our writes
  Spot game_safe{};       // the last spot the game left Snake standing, for respawns and skate off
  // Snake's actor is frozen while skating (game::freeze_snake). Where he got on the board, and
  // for a moment after skate off, a watch: if the game then drops him (no floor of its own
  // there, e.g. s091a's out-of-bounds strip), he goes back to where he got on.
  bool frozen = g_shared->frozen.control != nullptr;  // still frozen by the previous core
  game::Vec3 entry{g_shared->entry_mm[0], g_shared->entry_mm[1], g_shared->entry_mm[2]};
  int watch_ticks = 0;
  float watch_y = 0;    // his height at skate off
  int hold_ticks = 0;   // holding him on the entry spot while the game lands him there
  if (g_shared->resume) {
    // The previous core stopped mid-ride: back on the board where he was.
    g_shared->resume = false;
    request_at(g_shared->resume_mm[0], g_shared->resume_mm[1], g_shared->resume_mm[2], g_shared->resume_heading_deg);
    log("skate: resuming the ride after a reload");
  }
  int trace_left = 0;  // engine ticks still to trace (harness `trace`), to MetalGearSkate.trace.csv
  std::string trace_rows;
  Pacer pacer(period);
  while (!g_stop) {
    pacer.wait();
    if (const int n = g_trace_request.exchange(0); n > 0) {
      trace_left = n;
      trace_rows = "tick_ms,src,buttons,lx,ly,rx,ry,x,y,z,speed,state\n";
    }
    const bool key_down = (GetAsyncKeyState(kToggleKey) & 0x8000) != 0;
    if (const int want = g_stealth.exchange(-1); want >= 0) {
      stealth_enabled = want == 1;
      if (skating && stealth_enabled) stealth.begin(true);
      if (!stealth_enabled) stealth.end();
    }
    const int remote = g_request.exchange(-1);
    // A harness start at a given spot while already skating starts over there.
    if (remote == 2 && skating) skating = false;
    const bool toggled =
        (key_down && !key_was_down) || ((remote == 1 || remote == 2) && !skating) || (remote == 0 && skating);
    key_was_down = key_down;

    game::Vec3* snake = game::snake_position();
    if (toggled) {
      if (skating) {
        skating = false;
        if (game_falling > 0) {
          // Hand Snake back where the game's own floor holds him, or he falls (to his death).
          *snake = {game_safe.at[0] * kMmPerUnit, game_safe.at[1] * kMmPerUnit + kSnakeHeightMm,
                    game_safe.at[2] * kMmPerUnit};
          log("skate: no game floor under Snake; handing him back at %.2f %.2f %.2f (m)", game_safe.at[0],
              game_safe.at[1], game_safe.at[2]);
        } else if (snake && floor_fn) {
          // While skating he's held at the board's ride height (the floor + ~887 mm); the game
          // stands him at the floor + 800, and settling those few cm when it takes him back
          // looked like a fall to the drop watch below. Put him on the floor under the board.
          float floor_y = 0.f;
          if (floor_fn(session, st.skater[0], st.skater[1] + 0.5f, st.skater[2], &floor_y) == 1 &&
              st.skater[1] - floor_y < 0.5f) {
            snake->y = floor_y * kMmPerUnit + kSnakeHeightMm;
          }
        }
        game::block_input(false);
        pose::publish(nullptr);
        publish_pose(nullptr);
        camera::clear_poses();
        stealth.end();
        log("skate: off (life %d/%d, alerts %d)", game::link_short(game::kLinkCurrentHealth),
            game::link_short(game::kLinkMaxHealth), game::link_short(game::kLinkAlertCount));
      } else if (!snake) {
        log("skate: no player to put on a board");
      } else {
        StartAt start;
        {
          std::lock_guard lock(g_status_mutex);
          start = g_start_at;
          g_start_at.set = false;
        }
        // Where he stood when he got on (before any harness placement): ground the game holds.
        if (!frozen) entry = *snake;
        if (start.set) {
          // Place Snake first so the game's own state agrees with where the board goes down.
          *snake = {start.x, start.y, start.z};
          const int16_t mgs_yaw = static_cast<int16_t>(std::lround(start.heading_deg * 65536.f / 360.f));
          if (int16_t* yaw = game::snake_yaw()) *yaw = mgs_yaw;
          if (int16_t* target = game::snake_target_yaw()) *target = mgs_yaw;
        }
        const float x = snake->x / kMmPerUnit, z = snake->z / kMmPerUnit;
        const float y = (snake->y - kSnakeHeightMm) / kMmPerUnit;
        const float at[3] = {x, y, z};
        const int16_t* yaw = game::snake_yaw();
        const float heading = yaw ? headings.to_engine(*yaw) : 0.f;
        if (broken && replace_session()) broken = false;
        if (broken) {
          log("skate: engine session still loading after an error; try again in a few seconds");
        } else if (!begin(at, heading)) {
          broken = true;  // a failed start can leave the session unusable too
          if (replace_session()) broken = false;
        } else {
          skating = true;
          game::block_input(true);
          stealth.begin(stealth_enabled);
          ticks = 0;
          last_good = {{at[0], at[1], at[2]}, heading};
          riding_spot = {{}, NAN};
          placed = true;
          written_y = NAN;
          game_falling = 0;
          game_safe = last_good;
          log("skate: on at %.1f %.1f %.1f (mm), Snake yaw %d -> heading %.1f deg (life %d/%d, alerts %d)", snake->x,
              snake->y, snake->z, yaw ? *yaw : 0, heading * 180.f / std::numbers::pi_v<float>,
              game::link_short(game::kLinkCurrentHealth), game::link_short(game::kLinkMaxHealth),
              game::link_short(game::kLinkAlertCount));
        }
      }
    }
    st.skating = skating;
    game::engine_takes_injection(skating);
    audio::set_playing(skating);
    if (skating != frozen) {
      if (skating) {
        game::freeze_snake(true);
      } else {
        game::freeze_snake(false);
        watch_ticks = 30;
        hold_ticks = 0;
        if (snake) watch_y = snake->y;
      }
      frozen = skating;
    }
    // Caught early (any real drop in the first half second): once a fall has built up, MGS3
    // kills him even after he's moved onto solid ground.
    constexpr float kOffDropMm = 60.f;
    if (!skating && watch_ticks > 0 && snake) {
      --watch_ticks;
      if (watch_y - snake->y > kOffDropMm) {
        log("skate: the game is dropping Snake after skate off; back to where he got on (%.0f %.0f %.0f mm)",
            entry.x, entry.y, entry.z);
        watch_ticks = 0;
        hold_ticks = 30;
      }
    }
    if (!skating && hold_ticks > 0 && snake) {
      --hold_ticks;
      *snake = entry;
    }
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

    char pad_source = 0;
    const MgskControls pad = read_pad(&pad_source);
    float m[16];
    if (f.tick(session, &pad) || f.root(session, m)) {
      log("skate: engine error: %s", f.last_error());
      // Carry on from the last good spot on a fresh session, like a bail respawn.
      if (replace_session() && begin(last_good.at, last_good.heading)) {
        log("skate: resumed at %.2f %.2f %.2f (m)", last_good.at[0], last_good.at[1], last_good.at[2]);
        placed = true;
        continue;
      }
      broken = true;
      log("skate: off until a fresh engine session is ready");
      skating = false;
      game::block_input(false);
      pose::publish(nullptr);
      stealth.end();
      continue;
    }
    // Off the edge of the stage's collision the skater falls forever, and a Snake written far
    // below the floor dies (game over). Put him back where he last rode instead.
    float floor_y = 0.f;
    const int floor_found = floor_fn ? floor_fn(session, m[12], m[13] + kFloorProbe, m[14], &floor_y) : 1;
    const bool off_stage = floor_found == 0 || (floor_found == 1 && floor_fn && m[13] - floor_y > kMaxDrop);
    float spot_floor = 0.f;
    const bool spot_has_floor =
        !floor_fn || floor_fn(session, last_good.at[0], last_good.at[1] + kFloorProbe, last_good.at[2], &spot_floor) == 1;
    if ((off_stage || m[13] < last_good.at[1] - kFallLimit) && !spot_has_floor) {
      // The spot he'd go back to is off the stage too (e.g. a start placed outside it): stop
      // rather than respawn there forever.
      log("skate: no floor at %.2f %.2f %.2f either; off", last_good.at[0], last_good.at[1], last_good.at[2]);
      int expected = -1;
      g_request.compare_exchange_strong(expected, 0);  // a pending harness re-place wins
    } else if (off_stage || m[13] < last_good.at[1] - kFallLimit) {
      log("skate: left the stage at %.2f %.2f %.2f (floor %s); back to %.2f %.2f %.2f (m)", m[12], m[13], m[14],
          floor_found == 1 ? "far below" : "none", last_good.at[0], last_good.at[1], last_good.at[2]);
      if (f.activate(session, last_good.at, last_good.heading) || f.root(session, m)) {
        log("skate: respawn failed: %s", f.last_error());
        continue;
      }
      riding_spot = {{}, NAN};
      placed = true;
    }
    if (!std::isnan(written_y)) {
      if (written_y - snake->y > kGameDropMm) {
        ++game_falling;
      } else {
        game_falling = 0;
        game_safe = {{st.skater[0], st.skater[1], st.skater[2]}, st.heading_deg * std::numbers::pi_v<float> / 180.f};
      }
    }
    // Translation is the last column of the column-major root matrix.
    snake->x = m[12] * kMmPerUnit;
    snake->y = m[13] * kMmPerUnit + kSnakeHeightMm;
    snake->z = m[14] * kMmPerUnit;
    written_y = snake->y;
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
    const bool have_snake = posing && f.snake_pose(session, snake_bones, pose::kSnakeBones) == pose::kSnakeBones;
    if (f.state(session, st.engine_state, sizeof(st.engine_state)) < 0) st.engine_state[0] = 0;  // returns name length
    const bool have_board = pose_fn && pose_fn(session, pose, kBoardBodies) == kBoardBodies;
    if (have_board) carry.apply(f, session, st.engine_state, m, have_snake ? snake_bones : nullptr, pose);
    publish_pose(have_board ? pose : nullptr, have_camera ? camera : nullptr, have_snake ? snake_bones : nullptr);
    {
      const float dx = m[12] - st.skater[0], dz = m[14] - st.skater[2];
      st.speed_mps = placed ? 0.f : std::sqrt(dx * dx + dz * dz) / period;
      placed = false;
      note_recent(st.engine_state, m[13], st.speed_mps);
    }
    st.skater[0] = m[12];
    st.skater[1] = m[13];
    st.skater[2] = m[14];
    st.heading_deg = heading * 180.f / std::numbers::pi_v<float>;
    if (trace_left > 0) {
      char row[192];
      snprintf(row, sizeof(row), "%lu,%c,%04X,%d,%d,%d,%d,%.4f,%.4f,%.4f,%.3f,%s\n", GetTickCount(), pad_source,
               pad.buttons, pad.left[0], pad.left[1], pad.right[0], pad.right[1], m[12], m[13], m[14], st.speed_mps,
               st.engine_state);
      trace_rows += row;
      if (--trace_left == 0) std::ofstream(g_plugin_dir / L"MetalGearSkate.trace.csv", std::ios::trunc) << trace_rows;
    }
    // Respawn spot: a grounded position from 1-2 s ago, so it isn't the edge he just rolled off.
    if (ticks % 60 == 0) {
      if (!std::isnan(riding_spot.heading)) last_good = riding_spot;
      const bool grounded = strstr(st.engine_state, "Ground") && !strstr(st.engine_state, "Wipeout");
      riding_spot = grounded ? Spot{{m[12], m[13], m[14]}, heading} : Spot{{}, NAN};
    }
    stealth.hold();
    if (posing) pose::publish(have_snake ? snake_bones : nullptr);
    set_status(st);
    if (++ticks % 120 == 0 && log_verbose()) {
      const pose::Stats ps = pose::stats();
      log("skate: pose hook calls %llu, palette slots posed %llu in %llu Snake draws, last bone count %d, nearest "
          "57-bone object %.0f mm, relative %d",
          ps.calls, ps.applied, ps.draws, ps.last_count, ps.nearest_mm, ps.relative);
      log("skate: nearest 57-bone object %p at %.0f %.0f %.0f (mm)", ps.nearest_obj, ps.nearest_at[0],
          ps.nearest_at[1], ps.nearest_at[2]);
      log("skate: skater at %.2f %.2f %.2f (m) heading %.0f deg; pad buttons %04X left %d,%d right %d,%d", m[12], m[13], m[14],
          heading * 180.f / std::numbers::pi_v<float>, pad.buttons, pad.left[0], pad.left[1], pad.right[0], pad.right[1]);
    }
  }

  // Core unload: everything that points into this module goes, the session goes to the loader.
  if (skating) {
    // Snake stays frozen (the loader owns the stub); the next core puts him back on the board.
    if (const game::Vec3* snake = game::snake_position()) {
      g_shared->resume = true;
      g_shared->resume_mm[0] = snake->x;
      g_shared->resume_mm[1] = snake->y;
      g_shared->resume_mm[2] = snake->z;
      const int16_t* yaw = game::snake_yaw();
      g_shared->resume_heading_deg = yaw ? *yaw * 360.f / 65536.f : 0.f;
    } else {
      game::freeze_snake(false);
    }
  }
  game::engine_takes_injection(false);
  game::block_input(false);
  pose::publish(nullptr);
  publish_pose(nullptr);
  stealth.end();
  g_shared->entry_mm[0] = entry.x;
  g_shared->entry_mm[1] = entry.y;
  g_shared->entry_mm[2] = entry.z;
  spare.wait();
  g_shared->spare = spare.take();
  g_shared->session = session;
  g_shared->posing = posing;
  strncpy_s(g_shared->collision_stage, collision_stage.c_str(), _TRUNCATE);
  g_shared->collision_triangles = st.collision_triangles;
  g_shared->rails = st.rails;
  st = {};
  set_status(st);
  return 0;
}

}  // namespace

void request(bool on) { g_request = on ? 1 : 0; }

void set_stealth(bool on) { g_stealth = on ? 1 : 0; }

void trace(int ticks) { g_trace_request = ticks; }

void frame() {
  {
    std::lock_guard lock(g_frame_mutex);
    ++g_frames;
  }
  g_frame_cv.notify_one();
}

void request_at(float x_mm, float y_mm, float z_mm, float heading_deg) {
  {
    std::lock_guard lock(g_status_mutex);
    g_start_at = {true, x_mm, y_mm, z_mm, heading_deg};
  }
  g_request = 2;
}

bool camera(float (&out)[kCameraFloats]) {
  std::lock_guard lock(g_pose_mutex);
  if (g_camera_valid) memcpy(out, g_camera, sizeof(g_camera));
  return g_camera_valid;
}

bool frame_snapshot(float (&cam)[kCameraFloats], float (&board)[kBoardBodies * 16],
                    float (&snake)[pose::kSnakeBones * 16], uint64_t& tick) {
  std::lock_guard lock(g_pose_mutex);
  if (!g_camera_valid || !g_pose_valid || !g_snake_valid) return false;
  memcpy(cam, g_camera, sizeof(g_camera));
  memcpy(board, g_pose, sizeof(g_pose));
  memcpy(snake, g_snake, sizeof(g_snake));
  tick = g_pose_tick;
  return true;
}

bool tick_pose(float (&board)[kBoardBodies * 16], float (&snake)[pose::kSnakeBones * 16], uint64_t* tick) {
  std::lock_guard lock(g_pose_mutex);
  if (!g_pose_valid || !g_snake_valid) return false;
  memcpy(board, g_pose, sizeof(g_pose));
  memcpy(snake, g_snake, sizeof(g_snake));
  if (tick) *tick = g_pose_tick;
  return true;
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

Recent recent() {
  std::lock_guard lock(g_status_mutex);
  return g_recent_empty ? Recent{} : g_recent;
}

void reset_recent() {
  std::lock_guard lock(g_status_mutex);
  g_recent_empty = true;
}

void start(const std::filesystem::path& plugin_dir, HMODULE ffi, core::CoreShared* shared) {
  g_plugin_dir = plugin_dir;
  g_ffi_module = ffi;
  g_shared = shared;
  g_stop = false;
  g_thread = CreateThread(nullptr, 0, run, nullptr, 0, nullptr);
}

void stop() {
  g_stop = true;
  if (g_thread) {
    WaitForSingleObject(g_thread, INFINITE);
    CloseHandle(g_thread);
    g_thread = nullptr;
  }
  // The output thread (the session and its sound stay in the DLL for the next core).
  audio::stop();
}

}  // namespace skate
