#include "game.hpp"

#include <windows.h>
#include <intrin.h>
#include <psapi.h>

#include <atomic>
#include <deque>
#include <cmath>
#include <cstring>
#include <mutex>

#include <safetyhook.hpp>
#include <optional>
#include <string_view>
#include <vector>

#include "log.hpp"

namespace game {
namespace {

// Pointer to Snake's movement block; his position is at +0x10.
// Found on MC 3.0.0 as exe+1E16CD8 (exe+1E16CC0 points at the same actor, 0x120 earlier).
// Site: exe+AB455  mov rax,[rip+disp]; movsx ecx,word [rbx-8]; movsx edx,word [rax+0x42]; call
constexpr std::string_view kSnakeControlSig = "48 8B 05 ?? ?? ?? ?? 0F BF 4B F8 0F BF 50 42 E8";
constexpr size_t kSnakePositionOffset = 0x10;
// Found by turning Snake in place: both shorts change together.
constexpr size_t kSnakeYawOffset = 0x42;
constexpr size_t kSnakeTargetYawOffset = 0x4A;

uint8_t** g_snake_control = nullptr;

// Static camera object (docs/formats.md, "Runtime findings"): +0x40 look-at, +0x50 eye.
constexpr size_t kCameraRva = 0x1D39D80;
constexpr size_t kCameraLookAt = 0x40, kCameraEye = 0x50;

// Processed pads, 0x44 bytes each, filled from the raw Steam Input block by the per-frame pad
// update at exe+111DF0 (MC 3.0.0); this is its first instruction after the prologue:
// lea rbp,[pads]; mov r14d,ecx; mov [rsp+0x68],rbp
constexpr std::string_view kPadsSig = "48 8D 2D ?? ?? ?? ?? 44 8B F1 48 89 6C 24 68";
uint8_t* g_pads = nullptr;
constexpr size_t kPadSize = 0x44;  // processed pad record

// Per-pad copy at exe+111C80 (MC 3.0.0): copy(dst = rcx, src = rdx) takes one pad record as
// Steam Input left it (src, the 0x44-byte records at `pads`, layout = game::Pad) and copies
// src+4 into the 0x24-byte record the rest of the game reads (dst). The outer per-frame
// update calls it 0x2E bytes after the signature site.
constexpr size_t kPadCopyCallFromSig = 0x2E;
// Bytes in a source record that hold stick values (rest at 0x80 when idle). +0x22 is a second
// stick copy seen changing with the left stick; everything else from +4 is buttons/pressure.
constexpr size_t kSrcSticks = 0x08, kSrcSticks2 = 0x22;

SafetyHookInline g_pad_copy_hook;
std::mutex g_pad_mutex;
Pad g_pad_copy{};
bool g_pad_seen = false;
std::atomic<bool> g_block_input{false};

// Stage loading, from MGSHDFix (MIT, src/resources/game_funcs.cpp, MGS3 "EnterDeveloperMenu"):
// GM_SetArea(GM_SaveArea, name); GCL_ChangeSenerioCode(StrCode("scenerio")); GM_Result = 9999;
// *GM_LoadRequest = 3. Same signatures as MGSHDFix uses.
constexpr std::string_view kSetAreaCallSig = "E8 ?? ?? ?? ?? 48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? E8 ?? ?? ?? ?? E8";
constexpr std::string_view kScenarioSig =
    "83 0D ?? ?? ?? ?? ?? B9 ?? ?? ?? ?? E8 ?? ?? ?? ?? B9 ?? ?? ?? ?? 48 83 C4 ?? E9";
constexpr std::string_view kLinkVarBufSig = "4C 8B 05 ?? ?? ?? ?? 66 41 83 78 ?? 00";
constexpr size_t kLinkResult = 2, kLinkSaveArea = 16;  // short GM_Result, int GM_SaveArea

using SetAreaFn = void(__fastcall*)(int id, const char* dirname);
using ChangeScenarioFn = void(__fastcall*)(unsigned int code);
SetAreaFn g_set_area = nullptr;
ChangeScenarioFn g_change_scenario = nullptr;
int* g_load_request = nullptr;
uint8_t** g_linkvarbuf = nullptr;
char g_pending_stage[16] = {};

// Logging hooks to learn how the game itself loads stages (developer menu, story flow), so the
// harness can copy the exact arguments. Original functions are called through these.
SafetyHookInline g_set_area_hook, g_change_scenario_hook;

void __fastcall on_set_area(int id, const char* dirname) {
  short result = 0;
  int load_request = g_load_request ? *g_load_request : -1;
  if (g_linkvarbuf && *g_linkvarbuf) memcpy(&result, *g_linkvarbuf + 2, sizeof(result));
  // The caller tells a menu load (the game loop at exe+9D8B0 on MC 3.0.0, which takes the stage
  // name from the scenario stream) from ours (game::load_stage_now) and MGSHDFix's.
  const auto caller = reinterpret_cast<uint8_t*>(_ReturnAddress()) - reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr));
  log("game: GM_SetArea(id %d, \"%s\") GM_Result %d load_request %d, called from exe+%llX", id,
      dirname ? dirname : "(null)", result, load_request, static_cast<unsigned long long>(caller));
  g_set_area_hook.call<void>(id, dirname);
}

void __fastcall on_change_scenario(unsigned int code) {
  log("game: GCL_ChangeSenerioCode(0x%06X)", code);
  g_change_scenario_hook.call<void>(code);
}  // guarded by g_pad_mutex; empty = nothing pending

// KCEJ string hash (24-bit rotate-and-add), as in MGSHDFix GameVars::GV_StrCode.
constexpr uint32_t strcode(const char* text) {
  uint32_t h = 0;
  for (; *text; ++text) h = (((h << 5) | (h >> 19)) + static_cast<unsigned char>(*text)) & 0xFFFFFF;
  return h;
}

void load_stage_now(const char* name) {
  uint8_t* link = *g_linkvarbuf;
  int save_area;
  memcpy(&save_area, link + kLinkSaveArea, sizeof(save_area));
  g_set_area(save_area, name);
  g_change_scenario(strcode("scenerio"));
  const short result = 9999;
  memcpy(link + kLinkResult, &result, sizeof(result));
  // The developer menu loads stages with request 16 (seen via the GM_SetArea log hook);
  // MGSHDFix's "return to developer menu" uses 3. Request 3 for a real stage left Snake running
  // a scripted route after the load.
  *g_load_request = strcmp(name, "select") == 0 ? 3 : 16;
  log("game: loading stage %s", name);
}

// DualShock 2 pressure bytes in the source record: 12 bytes at +0x10 in the PS2 order right,
// left, up, down, triangle, circle, cross, square, L1, R1, L2, R2 (a real Cross press reads
// 0xFF at +0x16). Byte +6 reads 0x30 while a real button is held. The developer menu ignores
// injected presses without these.
constexpr size_t kSrcPressure = 0x10, kSrcHeldFlag = 0x06;
constexpr uint16_t kPressureOrder[12] = {0x0020, 0x0080, 0x0010, 0x0040, 0x1000, 0x2000,
                                         0x4000, 0x8000, 0x0400, 0x0800, 0x0100, 0x0200};

void write_pressure(uint8_t* src, uint16_t buttons) {
  for (int i = 0; i < 12; ++i) src[kSrcPressure + i] = (buttons & kPressureOrder[i]) ? 0xFF : 0x00;
  src[kSrcHeldFlag] = buttons ? 0x30 : 0x00;
}

struct Injection {
  uint16_t buttons = 0;
  uint8_t sticks[4] = {0x80, 0x80, 0x80, 0x80};  // RX RY LX LY
  int frames = 0;
};
std::deque<Injection> g_inject;  // guarded by g_pad_mutex; front plays first

void __fastcall on_pad_copy(uint8_t* dst, uint8_t* src) {
  if (src == g_pads) {  // pad 0
    {
      std::lock_guard lock(g_pad_mutex);
      // Only from the developer menu or a gameplay stage: a load during boot ("init", "title")
      // hangs the game in its script interpreter. Until then the request waits.
      const char* stage = current_stage();
      const bool can_load = stage && stage[0] && strcmp(stage, "init") != 0 && strcmp(stage, "title") != 0;
      if (g_pending_stage[0] && can_load) {
        load_stage_now(g_pending_stage);
        g_pending_stage[0] = 0;
      }
      while (!g_inject.empty() && g_inject.front().frames <= 0) g_inject.pop_front();
      if (!g_inject.empty()) {
        Injection& step = g_inject.front();
        --step.frames;
        memcpy(src + 4, &step.buttons, sizeof(uint16_t));
        write_pressure(src, step.buttons);
        memcpy(src + kSrcSticks, step.sticks, 4);
        memcpy(src + kSrcSticks2, step.sticks, 4);
      }
      memcpy(&g_pad_copy, src, sizeof(Pad));
      g_pad_seen = true;
    }
    if (g_block_input.load(std::memory_order_relaxed)) {
      // Idle the source before anything copies it: keep the status word, clear buttons and
      // pressure, centre the sticks. Steam Input rewrites it next poll.
      memset(src + 4, 0, kPadSize - 4);
      memset(src + kSrcSticks, 0x80, 4);
      memset(src + kSrcSticks2, 0x80, 4);
    }
  }
  g_pad_copy_hook.call<void>(dst, src);
}

// Current stage name (char[]); same signature MGSHDFix uses: lea r9,[stage]; sar rax,...
constexpr std::string_view kStageSig = "4C 8D 0D ?? ?? ?? ?? 48 C1 F8";
const char* g_stage = nullptr;

struct Pattern {
  std::vector<uint8_t> bytes;
  std::vector<bool> wild;
};

Pattern parse(std::string_view text) {
  Pattern p;
  for (size_t i = 0; i < text.size();) {
    if (text[i] == ' ') {
      ++i;
    } else if (text[i] == '?') {
      p.bytes.push_back(0);
      p.wild.push_back(true);
      i += 2;
    } else {
      p.bytes.push_back(static_cast<uint8_t>(strtoul(std::string(text.substr(i, 2)).c_str(), nullptr, 16)));
      p.wild.push_back(false);
      i += 2;
    }
  }
  return p;
}

std::optional<uint8_t*> scan(std::string_view signature) {
  const Pattern p = parse(signature);
  MODULEINFO info{};
  GetModuleInformation(GetCurrentProcess(), GetModuleHandleW(nullptr), &info, sizeof(info));
  auto* begin = static_cast<uint8_t*>(info.lpBaseOfDll);
  const size_t size = info.SizeOfImage;
  uint8_t* found = nullptr;
  for (size_t i = 0; i + p.bytes.size() <= size; ++i) {
    size_t j = 0;
    while (j < p.bytes.size() && (p.wild[j] || begin[i + j] == p.bytes[j])) ++j;
    if (j == p.bytes.size()) {
      if (found) return std::nullopt;  // not unique: refuse to guess
      found = begin + i;
    }
  }
  if (!found) return std::nullopt;
  return found;
}

// Target of a RIP-relative operand whose disp32 sits at `at` and whose instruction ends at `at + 4`.
uint8_t* rip_target(uint8_t* at) {
  int32_t disp;
  memcpy(&disp, at, sizeof(disp));
  return at + 4 + disp;
}

}  // namespace

bool init() {
  auto* exe = reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr));
  const auto site = scan(kSnakeControlSig);
  if (!site) {
    log("game: Snake control signature not found (or not unique); game version changed?");
    return false;
  }
  g_snake_control = reinterpret_cast<uint8_t**>(rip_target(*site + 3));
  if (const auto pads = scan(kPadsSig)) {
    g_pads = rip_target(*pads + 3);
    log("game: pads at exe+%llX", static_cast<unsigned long long>(g_pads - exe));
    uint8_t* call = *pads + kPadCopyCallFromSig;
    if (*call == 0xE8) {
      uint8_t* copy = rip_target(call + 1);
      g_pad_copy_hook = safetyhook::create_inline(copy, reinterpret_cast<void*>(&on_pad_copy));
      log("game: pad copy hook at exe+%llX %s", static_cast<unsigned long long>(copy - exe),
          g_pad_copy_hook ? "installed" : "FAILED");
    } else {
      log("game: pad copy call not where expected; no pad hook");
    }
  } else {
    log("game: pad signature not found (or not unique)");
  }
  if (const auto stage = scan(kStageSig)) {
    g_stage = reinterpret_cast<const char*>(rip_target(*stage + 3));
  } else {
    log("game: stage-name signature not found (or not unique)");
  }
  if (const auto call = scan(kSetAreaCallSig)) g_set_area = reinterpret_cast<SetAreaFn>(rip_target(*call + 1));
  if (const auto scenario = scan(kScenarioSig)) {
    g_change_scenario = reinterpret_cast<ChangeScenarioFn>(rip_target(*scenario + 0xD));
    // or dword [rip+disp32], imm8: disp at +2, instruction ends at +7.
    int32_t disp;
    memcpy(&disp, *scenario + 2, sizeof(disp));
    g_load_request = reinterpret_cast<int*>(*scenario + 7 + disp);
  }
  if (const auto link = scan(kLinkVarBufSig)) g_linkvarbuf = reinterpret_cast<uint8_t**>(rip_target(*link + 3));
  if (g_set_area) g_set_area_hook = safetyhook::create_inline(reinterpret_cast<void*>(g_set_area), reinterpret_cast<void*>(&on_set_area));
  if (g_change_scenario) {
    g_change_scenario_hook =
        safetyhook::create_inline(reinterpret_cast<void*>(g_change_scenario), reinterpret_cast<void*>(&on_change_scenario));
  }
  log("game: stage loading %s", g_set_area && g_change_scenario && g_load_request && g_linkvarbuf ? "ready" : "unavailable");
  log("game: Snake control pointer at exe+%llX (code exe+%llX)",
      static_cast<unsigned long long>(reinterpret_cast<uint8_t*>(g_snake_control) - exe),
      static_cast<unsigned long long>(*site - exe));
  return true;
}

bool pad(Pad& out) {
  std::lock_guard lock(g_pad_mutex);
  out = g_pad_copy;
  return g_pad_seen;
}

void block_input(bool blocked) { g_block_input = blocked; }

void inject_pad(uint16_t buttons, uint8_t lx, uint8_t ly, uint8_t rx, uint8_t ry, int frames) {
  std::lock_guard lock(g_pad_mutex);
  g_inject.clear();
  if (frames > 0) g_inject.push_back({buttons, {rx, ry, lx, ly}, frames});
}

bool request_stage(const char* name) {
  if (!g_set_area || !g_change_scenario || !g_load_request || !g_linkvarbuf) return false;
  if (strlen(name) >= sizeof(g_pending_stage)) return false;
  std::lock_guard lock(g_pad_mutex);
  strcpy_s(g_pending_stage, name);
  return true;
}

void queue_pad(uint16_t buttons, uint8_t lx, uint8_t ly, uint8_t rx, uint8_t ry, int frames) {
  std::lock_guard lock(g_pad_mutex);
  if (frames > 0) g_inject.push_back({buttons, {rx, ry, lx, ly}, frames});
}

int injected_frames_left() {
  std::lock_guard lock(g_pad_mutex);
  int left = 0;
  for (const Injection& step : g_inject) left += step.frames;
  return left;
}

const char* current_stage() { return g_stage; }

bool camera(Vec3& eye, Vec3& look_at) {
  const auto* object = reinterpret_cast<const uint8_t*>(GetModuleHandleW(nullptr)) + kCameraRva;
  memcpy(&look_at, object + kCameraLookAt, sizeof(Vec3));
  memcpy(&eye, object + kCameraEye, sizeof(Vec3));
  const auto sane = [](const Vec3& v) {
    for (const float c : {v.x, v.y, v.z}) {
      if (!std::isfinite(c) || std::fabs(c) > 1e7f) return false;
    }
    return true;
  };
  const float dx = eye.x - look_at.x, dy = eye.y - look_at.y, dz = eye.z - look_at.z;
  return sane(eye) && sane(look_at) && dx * dx + dy * dy + dz * dz > 1.f;
}

Vec3* snake_position() {
  if (!g_snake_control || !*g_snake_control) return nullptr;
  return reinterpret_cast<Vec3*>(*g_snake_control + kSnakePositionOffset);
}

int16_t* snake_yaw() {
  if (!g_snake_control || !*g_snake_control) return nullptr;
  return reinterpret_cast<int16_t*>(*g_snake_control + kSnakeYawOffset);
}

namespace {
// Snake's actor work around his control block (MC 3.0.0, docs/formats.md "Snake's actor"): the
// actor header's update function pointer sits 0x118 before the control block and his render
// object 0x970 after it; the object's bounding box max/min (float3 each) at +0x120/+0x130 and
// bounding radius at +0x14C (1253 mm for sna_def), bone count at +0xB0.
constexpr ptrdiff_t kActorUpdateSlot = -0x118;
constexpr size_t kRenderObject = 0x970, kBoundsMax = 0x120, kBoundsMin = 0x130, kBoundsRadius = 0x14C,
                 kObjBoneCount = 0xB0;
constexpr float kHugeBoundsMm = 1.0e7f;

void __fastcall frozen_update(void*) {}

struct Frozen {
  uint8_t* control = nullptr;
  void* update = nullptr;     // the actor's own update, put back on unfreeze
  uint8_t bounds[0x30] = {};  // +0x120..+0x14F as they were
} g_frozen;
}  // namespace

bool freeze_snake(bool frozen) {
  uint8_t* control = (g_snake_control && *g_snake_control) ? *g_snake_control : nullptr;
  if (!frozen) {
    if (!g_frozen.control) return true;
    // Only touch the actor we froze, and only if it is still the player's.
    if (control == g_frozen.control) {
      memcpy(control + kActorUpdateSlot, &g_frozen.update, sizeof(void*));
      memcpy(control + kRenderObject + kBoundsMax, g_frozen.bounds, sizeof(g_frozen.bounds));
    }
    g_frozen = {};
    return true;
  }
  if (g_frozen.control) return g_frozen.control == control;
  if (!control) return false;
  void* update = nullptr;
  memcpy(&update, control + kActorUpdateSlot, sizeof(update));
  const auto* exe = reinterpret_cast<const uint8_t*>(GetModuleHandleW(nullptr));
  int bones = 0;
  memcpy(&bones, control + kRenderObject + kObjBoneCount, sizeof(bones));
  float radius = 0;
  memcpy(&radius, control + kRenderObject + kBoundsRadius, sizeof(radius));
  const bool update_in_exe = update >= exe && update < exe + 0x2000000;
  if (!update_in_exe || bones != 57 || !(radius > 100.f && radius < 10000.f)) {
    log("game: Snake's actor isn't laid out as expected (update %p, bones %d, radius %.0f); not frozen", update,
        bones, radius);
    return false;
  }
  g_frozen.control = control;
  g_frozen.update = update;
  memcpy(g_frozen.bounds, control + kRenderObject + kBoundsMax, sizeof(g_frozen.bounds));
  void* stub = reinterpret_cast<void*>(&frozen_update);
  memcpy(control + kActorUpdateSlot, &stub, sizeof(stub));
  const float huge_max[3] = {kHugeBoundsMm, kHugeBoundsMm, kHugeBoundsMm};
  const float huge_min[3] = {-kHugeBoundsMm, -kHugeBoundsMm, -kHugeBoundsMm};
  memcpy(control + kRenderObject + kBoundsMax, huge_max, sizeof(huge_max));
  memcpy(control + kRenderObject + kBoundsMin, huge_min, sizeof(huge_min));
  memcpy(control + kRenderObject + kBoundsRadius, &kHugeBoundsMm, sizeof(float));
  return true;
}

int16_t* snake_target_yaw() {
  if (!g_snake_control || !*g_snake_control) return nullptr;
  return reinterpret_cast<int16_t*>(*g_snake_control + kSnakeTargetYawOffset);
}

uint8_t* linkvar() { return g_linkvarbuf ? *g_linkvarbuf : nullptr; }

uint8_t* find(std::string_view signature) {
  const auto hit = scan(signature);
  return hit ? *hit : nullptr;
}

}  // namespace game
