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
constexpr size_t kObjMatrix = 0x20, kObjTranslation = 0x50, kObjBoneCount = 0xB0, kObjBones = 0xC0;
// The pass then loads a global render context (mov rdx,[rip+disp] at +0xC, disp at +0xF) and adds
// its float3 at +0x1D0 to every bone translation: bones are stored relative to that origin (the
// camera's look-at point; MGS3 renders camera-relative). Our world-space pose must subtract it.
constexpr size_t kContextLoadDisp = 0xF, kContextOrigin = 0x1D0;
uint8_t** g_context = nullptr;
// Snake's object sits on his gameplay position; anything farther than this is someone else.
constexpr float kMaxDistanceMm = 2500.f;

SafetyHookInline g_hook;
std::mutex g_mutex;
float g_bones[kSnakeBones * 16];
bool g_active = false;  // guarded by g_mutex
float g_game_bones[kSnakeBones * 16];  // guarded by g_mutex: Snake's bones as the game left them
float g_game_origin[3];
float g_game_object[16];  // obj+0x20, the render object's world matrix
bool g_game_relative = false, g_game_valid = false;
std::atomic<uint64_t> g_calls{0}, g_applied{0}, g_draws{0};
std::atomic<int> g_last_count{0};

// Render origin (camera-relative rendering): the float3 the pass adds back to translations.
bool render_origin(float out[3]) {
  if (!g_context || !*g_context) return false;
  memcpy(out, *g_context + kContextOrigin, 3 * sizeof(float));
  return true;
}

std::atomic<float> g_nearest_mm{-1.f};  // closest 57-bone object to Snake this frame (diagnostics)
std::atomic<int> g_relative{-1};        // 1 = bones stored relative to the render origin, 0 = world

// Snake's render object sits on his gameplay position. Its translation may be stored in world
// space or relative to the render origin; accept either and report which (sets `relative`).
bool is_snake(const uint8_t* obj, const float origin[3], bool& relative) {
  const game::Vec3* snake = game::snake_position();
  if (!snake) return false;
  float t[3];
  memcpy(t, obj + kObjTranslation, sizeof(t));
  auto dist2 = [&](float ox, float oy, float oz) {
    const float dx = t[0] + ox - snake->x, dy = t[1] + oy - snake->y, dz = t[2] + oz - snake->z;
    return dx * dx + dy * dy + dz * dz;
  };
  const float world = dist2(0, 0, 0), rel = dist2(origin[0], origin[1], origin[2]);
  relative = rel < world;
  const float best = relative ? rel : world;
  const float nearest = g_nearest_mm.load(std::memory_order_relaxed);
  if (nearest < 0 || std::sqrt(best) < nearest) g_nearest_mm.store(std::sqrt(best), std::memory_order_relaxed);
  return best < kMaxDistanceMm * kMaxDistanceMm;
}

void __fastcall on_bone_pass(uint8_t* obj, void* arg2) {
  g_calls.fetch_add(1, std::memory_order_relaxed);
  int count = 0;
  memcpy(&count, obj + kObjBoneCount, sizeof(count));
  g_last_count.store(count, std::memory_order_relaxed);
  if (count == kSnakeBones) {
    std::lock_guard lock(g_mutex);
    float origin[3] = {0, 0, 0};
    render_origin(origin);
    bool relative = false;
    if (is_snake(obj, origin, relative)) {
      g_relative.store(relative ? 1 : 0, std::memory_order_relaxed);
      // Snake's bones for this frame, as the game computed them (the renderer matches his
      // palette slots against them). Overwriting them here has no visible effect: the GPU
      // palette is built from another copy.
      const float* bones = nullptr;
      memcpy(&bones, obj + kObjBones, sizeof(bones));
      if (bones) {
        memcpy(g_game_bones, bones, sizeof(g_game_bones));
        memcpy(g_game_origin, origin, sizeof(g_game_origin));
        memcpy(g_game_object, obj + kObjMatrix, sizeof(g_game_object));
        g_game_relative = relative;
        g_game_valid = true;
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
  int32_t disp;
  memcpy(&disp, site + kContextLoadDisp, sizeof(disp));
  g_context = reinterpret_cast<uint8_t**>(site + kContextLoadDisp + 4 + disp);
  g_hook = safetyhook::create_inline(site, reinterpret_cast<void*>(&on_bone_pass));
  log("pose: bone pass hook %s", g_hook ? "installed" : "FAILED");
  return static_cast<bool>(g_hook);
}

void publish(const float* bones) {
  std::lock_guard lock(g_mutex);
  g_active = bones != nullptr;
  if (bones) memcpy(g_bones, bones, sizeof(g_bones));
}

bool current(float* bones) {
  std::lock_guard lock(g_mutex);
  if (g_active) memcpy(bones, g_bones, sizeof(g_bones));
  return g_active;
}

void count_applied(uint64_t slots, uint64_t draws) {
  g_applied.fetch_add(slots, std::memory_order_relaxed);
  g_draws.fetch_add(draws, std::memory_order_relaxed);
}

bool game_bones(float* bones, float origin[3], float object[16], bool& relative) {
  std::lock_guard lock(g_mutex);
  if (!g_game_valid) return false;
  memcpy(bones, g_game_bones, sizeof(g_game_bones));
  memcpy(origin, g_game_origin, sizeof(g_game_origin));
  memcpy(object, g_game_object, sizeof(g_game_object));
  relative = g_game_relative;
  return true;
}

Stats stats() {
  Stats s;
  s.hooked = static_cast<bool>(g_hook);
  s.calls = g_calls.load();
  s.applied = g_applied.load();
  s.last_count = g_last_count.load();
  s.nearest_mm = g_nearest_mm.exchange(-1.f);
  s.relative = g_relative.load();
  s.draws = g_draws.load();
  return s;
}

}  // namespace pose
