#pragma once

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstring>

#include <safetyhook.hpp>

// Safe teardown for the core's inline hooks (the core is unloaded and loaded again while the game
// runs). Every function the game reaches through one of our hooks starts with `hooks::Guard g;`
// and, once `stopping()`, only calls the original. mgsk_core_stop then: marks stopping, disables
// every hook (original bytes back, trampolines kept, so a thread already on its way through one
// still has somewhere to go), waits until no thread is inside a hook function (`wait_idle`), and
// only then frees the hooks (reset) and what they used. If a thread stays inside, or another
// module hooked over one of ours, the core reports it isn't safe to free and the loader leaves it
// loaded (inert) instead.
namespace hooks {

inline std::atomic<int> g_in_flight{0};
inline std::atomic<bool> g_stopping{false};
inline std::atomic<bool> g_unsafe{false};  // a hook couldn't be removed cleanly

struct Guard {
  Guard() { g_in_flight.fetch_add(1, std::memory_order_acq_rel); }
  ~Guard() { g_in_flight.fetch_sub(1, std::memory_order_acq_rel); }
  Guard(const Guard&) = delete;
  Guard& operator=(const Guard&) = delete;
};

inline bool stopping() { return g_stopping.load(std::memory_order_acquire); }

// Whether the target still jumps to our hook (directly, or through safetyhook's near jump to an
// absolute one). False if another module re-hooked the function on top of ours.
inline bool still_ours(const SafetyHookInline& hook) {
  const uint8_t* target = hook.target();
  const uintptr_t dest = hook.destination_address();
  if (!target) return false;
  auto abs_jump = [](const uint8_t* at, uintptr_t& out) {
    if (at[0] == 0xFF && at[1] == 0x25) {  // jmp [rip+disp32]
      int32_t disp;
      memcpy(&disp, at + 2, 4);
      memcpy(&out, at + 6 + disp, 8);
      return true;
    }
    return false;
  };
  uintptr_t to = 0;
  if (target[0] == 0xE9) {
    int32_t rel;
    memcpy(&rel, target + 1, 4);
    to = reinterpret_cast<uintptr_t>(target) + 5 + rel;
    if (to == dest) return true;
    uintptr_t abs = 0;
    return abs_jump(reinterpret_cast<const uint8_t*>(to), abs) && abs == dest;
  }
  return abs_jump(target, to) && to == dest;
}

// Restores the original bytes, keeping the trampoline. Leaves (and flags) a hook another module
// has hooked over: restoring our saved bytes would cut its hook out.
inline void disable(SafetyHookInline& hook) {
  if (!hook || !hook.enabled()) return;
  if (!still_ours(hook)) {
    g_unsafe = true;
    return;
  }
  if (!hook.disable()) g_unsafe = true;
}

// Frees a disabled hook (its trampoline). A hook still enabled at this point (left by `disable`)
// stays: freeing it would leave the target jumping into freed memory.
inline void release(SafetyHookInline& hook) {
  if (!hook) return;
  if (hook.enabled()) {
    g_unsafe = true;
    return;
  }
  hook.reset();
}

}  // namespace hooks
