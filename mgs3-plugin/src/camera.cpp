#include "camera.hpp"

#include <windows.h>

#include <atomic>
#include <mutex>
#include <cmath>
#include <numbers>
#include <cstring>

#include <safetyhook.hpp>

#include "game.hpp"
#include "hooks.hpp"
#include "log.hpp"
#include "skate.hpp"

namespace camera {
namespace {

// The camera update (exe+AF050 on MC 3.0.0) copies a camera object's eye (+0x40) and target
// (+0x50) into working copies (+0x2D0, +0x2E0) and builds the view from them with a look-at
// (exe+C4290); the game's smoothing writes those two before the call. Found by its body (movups xmm0,[rcx+0x40]; mov rdi,[rcx+0x10];
// lea r14,[rcx+0x2e0]) at +0x73 into the function.
constexpr std::string_view kUpdateBodySig = "0F 10 41 40 48 8B 79 10 4C 8D B1 E0 02 00 00";
constexpr size_t kUpdateBodyOffset = 0x73;
// lea rax,[cameras]; cmp rax,rbx; jne: the camera table, whose entry 0 is the main camera.
constexpr std::string_view kMainCameraSig = "48 8D 05 ?? ?? ?? ?? 48 3B C3 0F 85";
constexpr size_t kEye = 0x40, kTarget = 0x50;
// Zoom factor (1.9 in s091a): the update multiplies it by +0xA0 into +0x32C, which goes to the
// view/projection setup with the view matrix. Rewritten by the game every frame.
constexpr size_t kZoom = 0x9C;
// Reference yaw (i16, 65536 = turn, atan2(dx, dz) from eye to target). After building the view
// the update turns the eye/target into angles (pitch +0x320, yaw +0x322) and, if the yaw is more
// than ~170 degrees (0x78E3) from this one or the pitch is over ~80 degrees (0x38E3), forces an
// 80-degree overhead view along it. A chase camera facing away from it would go top-down.
constexpr size_t kReferenceYaw = 0x92;
// The projection's vertical scale (cot of half the vertical fov) per unit of zoom: 2.434 at the
// game's 1.9 (s091a, 3440x1440). Used to give the game the skate camera's field of view.
constexpr float kScalePerZoom = 2.434f / 1.9f;
// The target: this far (mm) along the skate camera's forward from its eye (the direction alone
// sets the view; the distance just keeps it well conditioned).
constexpr float kTargetDistanceMm = 4000.f;

SafetyHookInline g_hook;
uint8_t* g_main = nullptr;
std::atomic<bool> g_driving{true};  // the default render camera (`drive`)
std::atomic<float> g_zoom{0.f};  // 0 = leave the game's
std::atomic<uint64_t> g_driven{0};
// Orbit (`render orbit <yaw,dist,height>`): the camera on a circle around the skater instead of the
// chase camera, for checking hands, head and board from the side or front. Yaw is relative to his
// heading: 0 = in front of him looking back, 90 = his right side, 180 = behind.

// The last few camera placements, each with the eye written and the board and Snake pose of the
// tick it came from. The frame the GPU draws uses the camera from one, two or sometimes three
// updates back (the pipeline latency varies; it varied most mid-strip in s091a, where Snake and
// the board then jumped a tick against the scene). Its camera reference L equals the eye written,
// so the renderer poses Snake from the placement whose eye is L (frame_pose_at). Guarded by
// g_snap_mutex.
struct Snap {
  float eye[3];
  float board[skate::kBoardBodies * 16];
  float snake[57 * 16];
  uint64_t tick;
  bool valid;
};
constexpr int kSnaps = 8;
std::mutex g_snap_mutex;
Snap g_snaps[kSnaps];
int g_snap_next = 0;  // the slot the next placement fills
Snap g_pending;       // this update's snapshot until its eye is written
std::atomic<bool> g_coherent{true};
float g_last_eye[3] = {NAN, NAN, NAN};  // the eye last written (guarded by g_snap_mutex; diagnostics)
float g_last_target[3] = {NAN, NAN, NAN};
// The camera's working eye/target after the game's own update ran (+0x2D0/+0x2E0 copies), to see
// whether the game moved it after our write (smoothing, collision). Guarded by g_snap_mutex.
float g_after_eye[3] = {NAN, NAN, NAN};
float g_after_target[3] = {NAN, NAN, NAN};
constexpr size_t kWorkEye = 0x2D0, kWorkTarget = 0x2E0;

std::atomic<bool> g_orbit{false};
std::atomic<float> g_orbit_yaw{90.f}, g_orbit_dist{2500.f}, g_orbit_height{1200.f};
// The look-at point: this far (mm) above the board.
constexpr float kOrbitLookMm = 900.f;

void __fastcall on_update(uint8_t* cam) {
  hooks::Guard guard;
  if (hooks::stopping()) return g_hook.call<void>(cam);
  if (g_driving.load(std::memory_order_relaxed) && cam == g_main) {
    float c[skate::kCameraFloats];
    bool have = false;
    {
      std::lock_guard lock(g_snap_mutex);
      g_pending.valid = skate::frame_snapshot(c, g_pending.board, g_pending.snake, g_pending.tick);
      have = g_pending.valid || skate::camera(c);
    }
    if (have) {
      // c: eye (3, mm), basis columns (side, up, forward), vertical fov (degrees).
      float eye[3] = {c[0], c[1], c[2]};
      const float* forward = c + 9;
      float look[3] = {eye[0] + forward[0] * kTargetDistanceMm, eye[1] + forward[1] * kTargetDistanceMm,
                       eye[2] + forward[2] * kTargetDistanceMm};
      if (g_orbit.load(std::memory_order_relaxed)) {
        const skate::Status st = skate::status();
        const float s[3] = {st.skater[0] * 1000.f, st.skater[1] * 1000.f, st.skater[2] * 1000.f};
        const float a = (st.heading_deg + g_orbit_yaw.load()) * std::numbers::pi_v<float> / 180.f;
        const float dist = g_orbit_dist.load();
        eye[0] = s[0] + std::sin(a) * dist;
        eye[1] = s[1] + g_orbit_height.load();
        eye[2] = s[2] + std::cos(a) * dist;
        look[0] = s[0];
        look[1] = s[1] + kOrbitLookMm;
        look[2] = s[2];
      }
      memcpy(cam + kEye, eye, sizeof(eye));
      {
        std::lock_guard lock(g_snap_mutex);
        memcpy(g_last_eye, eye, sizeof(eye));
        memcpy(g_last_target, look, sizeof(look));
        if (g_pending.valid) {
          memcpy(g_pending.eye, eye, sizeof(eye));
          g_snaps[g_snap_next] = g_pending;
          g_snap_next = (g_snap_next + 1) % kSnaps;
        }
      }
      memcpy(cam + kTarget, look, sizeof(look));
      const auto yaw = static_cast<int16_t>(
          std::lround(std::atan2(look[0] - eye[0], look[2] - eye[2]) * 32768.f / std::numbers::pi_v<float>));
      memcpy(cam + kReferenceYaw, &yaw, sizeof(yaw));
      float zoom = g_zoom.load(std::memory_order_relaxed);
      const float fov = c[12] * std::numbers::pi_v<float> / 180.f;
      if (zoom <= 0.f && fov > 0.1f && fov < 3.f) zoom = 1.f / std::tan(fov / 2.f) / kScalePerZoom;
      if (zoom > 0.f) memcpy(cam + kZoom, &zoom, sizeof(zoom));
      g_driven.fetch_add(1, std::memory_order_relaxed);
    }
  }
  g_hook.call<void>(cam);
  if (cam == g_main && g_driving.load(std::memory_order_relaxed)) {
    std::lock_guard lock(g_snap_mutex);
    memcpy(g_after_eye, cam + kWorkEye, sizeof(g_after_eye));
    memcpy(g_after_target, cam + kWorkTarget, sizeof(g_after_target));
  }
}

}  // namespace

bool init() {
  uint8_t* body = game::find(kUpdateBodySig);
  uint8_t* main_ref = game::find(kMainCameraSig);
  if (!body || !main_ref) {
    log("camera: update or camera table signature not found; the game camera can't follow the skater");
    return false;
  }
  int32_t disp;
  memcpy(&disp, main_ref + 3, sizeof(disp));
  g_main = main_ref + 7 + disp;
  g_hook = safetyhook::create_inline(body - kUpdateBodyOffset, reinterpret_cast<void*>(&on_update));
  log("camera: update hook %s, main camera at exe+%llX", g_hook ? "installed" : "FAILED",
      static_cast<unsigned long long>(g_main - reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr))));
  return static_cast<bool>(g_hook);
}

void disable_hooks() { hooks::disable(g_hook); }

void stop() { hooks::release(g_hook); }

void set_driving(bool on) { g_driving = on; }

void set_zoom(float zoom) { g_zoom = zoom; }

void set_orbit(bool on, float yaw_deg, float dist_mm, float height_mm) {
  g_orbit_yaw = yaw_deg;
  g_orbit_dist = dist_mm;
  g_orbit_height = height_mm;
  g_orbit = on;
}

bool orbiting() { return g_orbit; }

bool frame_pose_at(const float (&L)[3], float (&board)[skate::kBoardBodies * 16], float (&snake)[57 * 16],
                   uint64_t& tick, float& error_mm) {
  if (!g_coherent || !driving()) return false;
  std::lock_guard lock(g_snap_mutex);
  const Snap* best = nullptr;
  float best_d2 = 0;
  for (const Snap& s : g_snaps) {
    if (!s.valid) continue;
    const float dx = s.eye[0] - L[0], dy = s.eye[1] - L[1], dz = s.eye[2] - L[2];
    const float d2 = dx * dx + dy * dy + dz * dz;
    if (!best || d2 < best_d2) {
      best = &s;
      best_d2 = d2;
    }
  }
  if (!best) return false;
  memcpy(board, best->board, sizeof(best->board));
  memcpy(snake, best->snake, sizeof(best->snake));
  tick = best->tick;
  error_mm = std::sqrt(best_d2);
  return true;
}

void set_coherent(bool on) { g_coherent = on; }

bool camera_after(float (&eye)[3], float (&target)[3], float (&wrote_eye)[3], float (&wrote_target)[3]) {
  std::lock_guard lock(g_snap_mutex);
  memcpy(eye, g_after_eye, sizeof(eye));
  memcpy(target, g_after_target, sizeof(target));
  memcpy(wrote_eye, g_last_eye, sizeof(wrote_eye));
  memcpy(wrote_target, g_last_target, sizeof(wrote_target));
  return !std::isnan(eye[0]) && !std::isnan(wrote_eye[0]);
}

bool last_eye(float (&eye)[3]) {
  std::lock_guard lock(g_snap_mutex);
  memcpy(eye, g_last_eye, sizeof(eye));
  return !std::isnan(eye[0]);
}

uint64_t camera_tick() {
  std::lock_guard lock(g_snap_mutex);
  const Snap& last = g_snaps[(g_snap_next + kSnaps - 1) % kSnaps];
  return last.valid ? last.tick : 0;
}

bool coherent() { return g_coherent; }

bool driving() { return g_driving && g_hook; }

uint64_t driven() { return g_driven.load(); }

}  // namespace camera
