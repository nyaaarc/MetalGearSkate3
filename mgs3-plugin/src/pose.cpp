#include "pose.hpp"

#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>

#include <safetyhook.hpp>

#include "game.hpp"
#include "hooks.hpp"
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
// Ring of recent passes (guarded by g_mutex): g_hist_next is the slot the next pass writes.
float g_hist_bones[kBoneHistory][kSnakeBones * 16];
float g_hist_object[kBoneHistory][16];
int g_hist_count = 0, g_hist_next = 0;
std::atomic<uint64_t> g_calls{0}, g_applied{0}, g_draws{0};
std::atomic<int> g_last_count{0};

// Render origin (camera-relative rendering): the float3 the pass adds back to translations.
bool render_origin(float out[3]) {
  if (!g_context || !*g_context) return false;
  memcpy(out, *g_context + kContextOrigin, 3 * sizeof(float));
  return true;
}

std::atomic<float> g_nearest_mm{-1.f};  // closest 57-bone object to Snake this frame (diagnostics)
float g_nearest_at[3] = {0, 0, 0};       // its translation (diagnostics, racy by design)
const void* g_nearest_obj = nullptr;
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
  if (nearest < 0 || std::sqrt(best) < nearest) {
    g_nearest_mm.store(std::sqrt(best), std::memory_order_relaxed);
    memcpy(g_nearest_at, t, sizeof(t));
    g_nearest_obj = obj;
  }
  return best < kMaxDistanceMm * kMaxDistanceMm;
}

// Render objects attached to Snake (held weapon, items) while he is frozen for skating. They hang
// from his frozen object (where he got on the board), so far from there the game culls them by
// their own bounds and the held weapon isn't drawn at all. Their bounds are made huge, like his
// (game::freeze_snake), and put back when he stops. Guarded by g_mutex.
constexpr size_t kObjModel = 0x00, kObjBounds = 0x120, kObjBoundsSize = 0x30, kObjRadius = 0x14C;
constexpr float kAttachedMm = 2500.f, kHugeBoundsMm = 1.0e7f;
struct Inflated {
  uint8_t* obj = nullptr;
  void* model = nullptr;
  uint8_t bounds[kObjBoundsSize] = {};
};
std::array<Inflated, 16> g_inflated;
int g_inflated_count = 0;

// Bounds that look like a render object's: max >= min on each axis and a plausible radius.
bool plausible_bounds(const uint8_t* obj) {
  float b[6], radius = 0;
  memcpy(b, obj + kObjBounds, sizeof(b));
  memcpy(&radius, obj + kObjRadius, sizeof(radius));
  for (int i = 0; i < 3; ++i) {
    if (!std::isfinite(b[i]) || !std::isfinite(b[3 + i]) || b[i] < b[3 + i] || std::fabs(b[i]) > 1e6f) return false;
  }
  return radius > 1.f && radius < 20000.f;
}

void keep_drawn(uint8_t* obj, int count) {
  for (int i = 0; i < g_inflated_count; ++i) {
    if (g_inflated[i].obj == obj) return;
  }
  if (g_inflated_count >= static_cast<int>(g_inflated.size()) || !plausible_bounds(obj)) return;
  Inflated& e = g_inflated[g_inflated_count++];
  e.obj = obj;
  memcpy(&e.model, obj + kObjModel, sizeof(e.model));
  memcpy(e.bounds, obj + kObjBounds, sizeof(e.bounds));
  const float huge_max[3] = {kHugeBoundsMm, kHugeBoundsMm, kHugeBoundsMm};
  const float huge_min[3] = {-kHugeBoundsMm, -kHugeBoundsMm, -kHugeBoundsMm};
  memcpy(obj + kObjBounds, huge_max, sizeof(huge_max));
  memcpy(obj + kObjBounds + 0x10, huge_min, sizeof(huge_min));
  memcpy(obj + kObjRadius, &kHugeBoundsMm, sizeof(float));
  log("pose: keeping attached object %p (bones %d) drawn while skating", static_cast<void*>(obj), count);
}

// Puts their bounds back (only on objects that still look like the ones we changed).
void release_attached() {
  for (int i = 0; i < g_inflated_count; ++i) {
    Inflated& e = g_inflated[i];
    void* model = nullptr;
    float radius = 0;
    memcpy(&model, e.obj + kObjModel, sizeof(model));
    memcpy(&radius, e.obj + kObjRadius, sizeof(radius));
    if (model == e.model && radius == kHugeBoundsMm) memcpy(e.obj + kObjBounds, e.bounds, sizeof(e.bounds));
    e = {};
  }
  g_inflated_count = 0;
}

void __fastcall on_bone_pass(uint8_t* obj, void* arg2) {
  hooks::Guard guard;
  if (hooks::stopping()) return g_hook.call<void>(obj, arg2);
  g_calls.fetch_add(1, std::memory_order_relaxed);
  int count = 0;
  memcpy(&count, obj + kObjBoneCount, sizeof(count));
  g_last_count.store(count, std::memory_order_relaxed);
  if (const uint8_t* snake_obj = game::snake_render_object(); snake_obj && obj != snake_obj) {
    std::lock_guard lock(g_mutex);
    if (game::snake_frozen()) {
      float a[3], b[3];
      memcpy(a, obj + kObjTranslation, sizeof(a));
      memcpy(b, snake_obj + kObjTranslation, sizeof(b));
      const float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
      if (dx * dx + dy * dy + dz * dz < kAttachedMm * kAttachedMm) keep_drawn(obj, count);
    } else if (g_inflated_count) {
      release_attached();
    }
  }
  if (count == kSnakeBones) {
    std::lock_guard lock(g_mutex);
    float origin[3] = {0, 0, 0};
    render_origin(origin);
    bool relative = false;
    // His own render object, wherever it sits: frozen for skating it stays where he got on the
    // board (a core loaded mid-ride never sees it near him). Else the one on his position.
    const bool close_by = is_snake(obj, origin, relative);
    if (obj == game::snake_render_object() || close_by) {
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
        memcpy(g_hist_bones[g_hist_next], bones, sizeof(g_hist_bones[0]));
        memcpy(g_hist_object[g_hist_next], obj + kObjMatrix, sizeof(g_hist_object[0]));
        g_hist_next = (g_hist_next + 1) % kBoneHistory;
        g_hist_count = std::min(g_hist_count + 1, kBoneHistory);
      }
    }
  }
  g_hook.call<void>(obj, arg2);
}

}  // namespace

void disable_hooks() { hooks::disable(g_hook); }

void stop() {
  hooks::release(g_hook);
  std::lock_guard lock(g_mutex);
  release_attached();
  g_active = false;
}

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

int game_bone_history(float (*bones)[kSnakeBones * 16], float (*objects)[16], int max) {
  std::lock_guard lock(g_mutex);
  const int n = std::min(max, g_hist_count);
  for (int i = 0; i < n; ++i) {
    const int at = (g_hist_next - 1 - i + kBoneHistory) % kBoneHistory;
    memcpy(bones[i], g_hist_bones[at], sizeof(g_hist_bones[0]));
    memcpy(objects[i], g_hist_object[at], sizeof(g_hist_object[0]));
  }
  return n;
}

Stats stats() {
  Stats s;
  s.hooked = static_cast<bool>(g_hook);
  s.calls = g_calls.load();
  s.applied = g_applied.load();
  s.last_count = g_last_count.load();
  s.nearest_mm = g_nearest_mm.exchange(-1.f);
  memcpy(s.nearest_at, g_nearest_at, sizeof(s.nearest_at));
  s.nearest_obj = g_nearest_obj;
  s.relative = g_relative.load();
  s.draws = g_draws.load();
  return s;
}

}  // namespace pose
