#include "game.hpp"

#include <windows.h>
#include <psapi.h>

#include <atomic>
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

struct Injection {
  uint16_t buttons = 0;
  uint8_t sticks[4] = {0x80, 0x80, 0x80, 0x80};  // RX RY LX LY
  int frames = 0;
};
Injection g_inject;  // guarded by g_pad_mutex

void __fastcall on_pad_copy(uint8_t* dst, uint8_t* src) {
  if (src == g_pads) {  // pad 0
    {
      std::lock_guard lock(g_pad_mutex);
      if (g_inject.frames > 0) {
        --g_inject.frames;
        memcpy(src + 4, &g_inject.buttons, sizeof(uint16_t));
        memcpy(src + kSrcSticks, g_inject.sticks, 4);
        memcpy(src + kSrcSticks2, g_inject.sticks, 4);
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
  g_inject = {buttons, {rx, ry, lx, ly}, frames};
}

int injected_frames_left() {
  std::lock_guard lock(g_pad_mutex);
  return g_inject.frames;
}

const char* current_stage() { return g_stage; }

Vec3* snake_position() {
  if (!g_snake_control || !*g_snake_control) return nullptr;
  return reinterpret_cast<Vec3*>(*g_snake_control + kSnakePositionOffset);
}

int16_t* snake_yaw() {
  if (!g_snake_control || !*g_snake_control) return nullptr;
  return reinterpret_cast<int16_t*>(*g_snake_control + kSnakeYawOffset);
}

int16_t* snake_target_yaw() {
  if (!g_snake_control || !*g_snake_control) return nullptr;
  return reinterpret_cast<int16_t*>(*g_snake_control + kSnakeTargetYawOffset);
}

}  // namespace game
