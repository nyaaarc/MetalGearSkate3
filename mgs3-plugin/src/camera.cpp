#include "camera.hpp"

#include <windows.h>

#include <atomic>
#include <cmath>
#include <numbers>
#include <cstring>

#include <safetyhook.hpp>

#include "game.hpp"
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

void __fastcall on_update(uint8_t* cam) {
  if (g_driving.load(std::memory_order_relaxed) && cam == g_main) {
    float c[skate::kCameraFloats];
    if (skate::camera(c)) {
      // c: eye (3, mm), basis columns (side, up, forward), vertical fov (degrees).
      const float eye[3] = {c[0], c[1], c[2]};
      const float* forward = c + 9;
      const float look[3] = {eye[0] + forward[0] * kTargetDistanceMm, eye[1] + forward[1] * kTargetDistanceMm,
                             eye[2] + forward[2] * kTargetDistanceMm};
      memcpy(cam + kEye, eye, sizeof(eye));
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

void set_driving(bool on) { g_driving = on; }

void set_zoom(float zoom) { g_zoom = zoom; }

bool driving() { return g_driving && g_hook; }

uint64_t driven() { return g_driven.load(); }

}  // namespace camera
