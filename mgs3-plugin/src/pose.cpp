#include "pose.hpp"

#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>

#include <safetyhook.hpp>

#include "game.hpp"
#include "log.hpp"

namespace pose {
namespace {

// Start of the render-object bone pass: sub rsp,0x58; movaps [rsp+0x40],xmm6; mov r10,rdx;
// mov rdx,[rip+...]. rcx = render object: +0x50 world translation (float3, mm), +0xB0 bone
// count, +0xC0 pointer to the bone matrices (docs/formats.md, "Snake's render object").
constexpr std::string_view kBonePassSig = "48 83 EC 58 0F 29 74 24 40 4C 8B D2 48 8B 15";
constexpr size_t kObjTranslation = 0x50, kObjBoneCount = 0xB0, kObjBones = 0xC0;
// Snake's object sits on his gameplay position; anything farther than this is someone else.
constexpr float kMaxDistanceMm = 2500.f;

SafetyHookInline g_hook;
std::mutex g_mutex;
float g_bones[kSnakeBones * 16];
bool g_active = false;  // guarded by g_mutex
std::atomic<uint64_t> g_calls{0}, g_applied{0};
std::atomic<int> g_last_count{0};

bool is_snake(const uint8_t* obj) {
  const game::Vec3* snake = game::snake_position();
  if (!snake) return false;
  float t[3];
  memcpy(t, obj + kObjTranslation, sizeof(t));
  const float dx = t[0] - snake->x, dy = t[1] - snake->y, dz = t[2] - snake->z;
  return dx * dx + dy * dy + dz * dz < kMaxDistanceMm * kMaxDistanceMm;
}

void __fastcall on_bone_pass(uint8_t* obj, void* arg2) {
  g_calls.fetch_add(1, std::memory_order_relaxed);
  int count = 0;
  memcpy(&count, obj + kObjBoneCount, sizeof(count));
  g_last_count.store(count, std::memory_order_relaxed);
  if (count == kSnakeBones) {
    std::lock_guard lock(g_mutex);
    if (g_active && is_snake(obj)) {
      float* bones = nullptr;
      memcpy(&bones, obj + kObjBones, sizeof(bones));
      if (bones) {
        memcpy(bones, g_bones, sizeof(g_bones));
        g_applied.fetch_add(1, std::memory_order_relaxed);
      }
    }
  }
  g_hook.call<void>(obj, arg2);
}

}  // namespace

bool init() {
  uint8_t* site = game::find(kBonePassSig);
  if (!site) {
    log("pose: bone pass signature not found (or not unique); Snake won't be posed");
    return false;
  }
  g_hook = safetyhook::create_inline(site, reinterpret_cast<void*>(&on_bone_pass));
  log("pose: bone pass hook %s", g_hook ? "installed" : "FAILED");
  return static_cast<bool>(g_hook);
}

void publish(const float* bones) {
  std::lock_guard lock(g_mutex);
  g_active = bones != nullptr;
  if (bones) memcpy(g_bones, bones, sizeof(g_bones));
}

Stats stats() {
  Stats s;
  s.hooked = static_cast<bool>(g_hook);
  s.calls = g_calls.load();
  s.applied = g_applied.load();
  s.last_count = g_last_count.load();
  return s;
}

}  // namespace pose
