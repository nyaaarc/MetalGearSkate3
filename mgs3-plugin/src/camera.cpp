#include "camera.hpp"

#include <windows.h>

#include <atomic>
#include <cstring>

#include <safetyhook.hpp>

#include "game.hpp"
#include "log.hpp"
#include "skate.hpp"

namespace camera {
namespace {

// The camera update (exe+AF050 on MC 3.0.0) copies a camera object's look-at point (+0x40) and
// eye (+0x50) into working copies and builds the view from them; the game's smoothing writes
// those two before the call. Found by its body (movups xmm0,[rcx+0x40]; mov rdi,[rcx+0x10];
// lea r14,[rcx+0x2e0]) at +0x73 into the function.
constexpr std::string_view kUpdateBodySig = "0F 10 41 40 48 8B 79 10 4C 8D B1 E0 02 00 00";
constexpr size_t kUpdateBodyOffset = 0x73;
// lea rax,[cameras]; cmp rax,rbx; jne: the camera table, whose entry 0 is the main camera.
constexpr std::string_view kMainCameraSig = "48 8D 05 ?? ?? ?? ?? 48 3B C3 0F 85";
constexpr size_t kLookAt = 0x40, kEye = 0x50;
// How far ahead of the eye the look-at point goes (mm).
constexpr float kLookDistanceMm = 3000.f;

SafetyHookInline g_hook;
uint8_t* g_main = nullptr;
std::atomic<bool> g_driving{false};
std::atomic<uint64_t> g_driven{0};

void __fastcall on_update(uint8_t* cam) {
  if (g_driving.load(std::memory_order_relaxed) && cam == g_main) {
    float c[skate::kCameraFloats];
    if (skate::camera(c)) {
      // c: eye (3, mm), basis columns (side, up, forward), vertical fov (degrees).
      const float eye[3] = {c[0], c[1], c[2]};
      const float* forward = c + 9;
      const float look[3] = {eye[0] + forward[0] * kLookDistanceMm, eye[1] + forward[1] * kLookDistanceMm,
                             eye[2] + forward[2] * kLookDistanceMm};
      memcpy(cam + kLookAt, look, sizeof(look));
      memcpy(cam + kEye, eye, sizeof(eye));
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

bool driving() { return g_driving && g_hook; }

uint64_t driven() { return g_driven.load(); }

}  // namespace camera
