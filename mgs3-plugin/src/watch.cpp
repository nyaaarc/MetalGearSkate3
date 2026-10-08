#include "watch.hpp"

#include <windows.h>
#include <tlhelp32.h>

#include <atomic>
#include <cstdint>
#include <fstream>
#include <mutex>
#include <set>
#include <string>

#include "log.hpp"

namespace {

constexpr size_t kMaxReported = 32;

std::filesystem::path g_file;
std::atomic<uintptr_t> g_armed{0};
DWORD g_poll_thread = 0;
std::mutex g_seen_mutex;
std::set<uintptr_t> g_seen;

// DR7: enable DR0 locally, break on data writes (RW0=01), 4 bytes (LEN0=11).
constexpr DWORD64 kDr7Write4 = 1 | (1ull << 16) | (3ull << 18);

void set_all_threads(uintptr_t address) {
  HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
  if (snapshot == INVALID_HANDLE_VALUE) return;
  THREADENTRY32 entry{sizeof(entry)};
  const DWORD pid = GetCurrentProcessId();
  for (BOOL ok = Thread32First(snapshot, &entry); ok; ok = Thread32Next(snapshot, &entry)) {
    // A thread can't change its own debug registers while running; the poll thread never writes game state.
    if (entry.th32OwnerProcessID != pid || entry.th32ThreadID == g_poll_thread) continue;
    HANDLE thread = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME, FALSE,
                               entry.th32ThreadID);
    if (!thread) continue;
    if (SuspendThread(thread) != static_cast<DWORD>(-1)) {
      CONTEXT ctx{};
      ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
      if (GetThreadContext(thread, &ctx)) {
        ctx.Dr0 = address;
        ctx.Dr7 = address ? (ctx.Dr7 & ~0xF0003ull) | kDr7Write4 : ctx.Dr7 & ~0xF0003ull;
        SetThreadContext(thread, &ctx);
      }
      ResumeThread(thread);
    }
    CloseHandle(thread);
  }
  CloseHandle(snapshot);
}

void describe(uintptr_t address, char* out, size_t size) {
  HMODULE module = nullptr;
  wchar_t name[MAX_PATH] = L"?";
  if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                         reinterpret_cast<LPCWSTR>(address), &module)) {
    GetModuleFileNameW(module, name, MAX_PATH);
    snprintf(out, size, "%ls+%llX", std::filesystem::path(name).filename().c_str(),
             static_cast<unsigned long long>(address - reinterpret_cast<uintptr_t>(module)));
  } else {
    snprintf(out, size, "%p", reinterpret_cast<void*>(address));
  }
}

LONG CALLBACK on_exception(EXCEPTION_POINTERS* info) {
  if (info->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP) return EXCEPTION_CONTINUE_SEARCH;
  CONTEXT* c = info->ContextRecord;
  if (!(c->Dr6 & 1)) return EXCEPTION_CONTINUE_SEARCH;
  c->Dr6 = 0;
  // Data breakpoints trap after the write: Rip is the next instruction.
  const uintptr_t rip = c->Rip;
  {
    std::lock_guard lock(g_seen_mutex);
    if (g_seen.size() >= kMaxReported || !g_seen.insert(rip).second) return EXCEPTION_CONTINUE_EXECUTION;
  }
  char where[MAX_PATH + 32];
  describe(rip, where, sizeof(where));
  log("watch: write to %p, next instruction %s, thread %lu", reinterpret_cast<void*>(g_armed.load()), where,
      GetCurrentThreadId());
  log("  rax=%llx rbx=%llx rcx=%llx rdx=%llx rsi=%llx rdi=%llx rbp=%llx rsp=%llx", c->Rax, c->Rbx, c->Rcx, c->Rdx,
      c->Rsi, c->Rdi, c->Rbp, c->Rsp);
  log("  r8=%llx r9=%llx r10=%llx r11=%llx r12=%llx r13=%llx r14=%llx r15=%llx", c->R8, c->R9, c->R10, c->R11,
      c->R12, c->R13, c->R14, c->R15);
  // Code bytes around the writer, for a signature. The instruction itself ends at Rip.
  char bytes[3 * 48 + 1] = {};
  const auto* code = reinterpret_cast<const uint8_t*>(rip - 32);
  for (int i = 0; i < 48; ++i) snprintf(bytes + 3 * i, 4, "%02X ", code[i]);
  log("  code[rip-32..rip+16]: %s", bytes);
  return EXCEPTION_CONTINUE_EXECUTION;
}

uintptr_t read_target() {
  std::ifstream in(g_file);
  std::string text;
  if (!(in >> text)) return 0;
  try {
    return static_cast<uintptr_t>(std::stoull(text, nullptr, 16));
  } catch (...) {
    return 0;
  }
}

DWORD WINAPI poll(LPVOID) {
  g_poll_thread = GetCurrentThreadId();
  for (;;) {
    const uintptr_t wanted = std::filesystem::exists(g_file) ? read_target() : 0;
    const bool changed = wanted != g_armed.load();
    if (changed) {
      {
        std::lock_guard lock(g_seen_mutex);
        g_seen.clear();
      }
      g_armed = wanted;
      if (wanted) {
        log("watch: armed on %p", reinterpret_cast<void*>(wanted));
      } else {
        log("watch: disarmed");
      }
    }
    // While armed, re-apply every poll so threads created since are covered too.
    if (changed || wanted) set_all_threads(wanted);
    Sleep(500);
  }
}

}  // namespace

void watch_start(const std::filesystem::path& dir) {
  g_file = dir / L"MetalGearSkate.watch";
  AddVectoredExceptionHandler(1, on_exception);
  if (HANDLE thread = CreateThread(nullptr, 0, poll, nullptr, 0, nullptr)) CloseHandle(thread);
}
