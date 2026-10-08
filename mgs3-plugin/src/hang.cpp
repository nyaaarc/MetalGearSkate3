#include "hang.hpp"

#include <windows.h>
#include <dbghelp.h>
#include <tlhelp32.h>

#include <vector>

#include "log.hpp"
#include "render.hpp"

#pragma comment(lib, "dbghelp.lib")

namespace hang {
namespace {

std::filesystem::path g_dir;
// No new frame for this long (s) while the game had been presenting means it hung. Stage loads
// stop Present for a few seconds at most.
constexpr int kStaleSeconds = 8;
constexpr int kMaxFrames = 32;

struct ThreadStack {
  DWORD id = 0;
  int count = 0;
  DWORD64 rip[kMaxFrames] = {};
};

// Walks a suspended thread's stack with the x64 unwind tables. Takes no locks of its own, so it is
// safe while other threads are frozen; module names are looked up after they are resumed.
void walk_frames(CONTEXT& ctx, ThreadStack& out) {
  for (out.count = 0; out.count < kMaxFrames && ctx.Rip; ++out.count) {
    out.rip[out.count] = ctx.Rip;
    DWORD64 image = 0;
    PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(ctx.Rip, &image, nullptr);
    if (!fn) {  // leaf function: the return address is at the top of the stack
      ctx.Rip = *reinterpret_cast<DWORD64*>(ctx.Rsp);
      ctx.Rsp += 8;
      continue;
    }
    void* handler_data = nullptr;
    DWORD64 frame = 0;
    RtlVirtualUnwind(UNW_FLAG_NHANDLER, image, ctx.Rip, fn, &ctx, &handler_data, &frame, nullptr);
  }
}

void walk(CONTEXT ctx, ThreadStack& out) {
  __try {
    walk_frames(ctx, out);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    // A corrupt or foreign stack: keep the frames found so far.
  }
}

void describe(DWORD64 address, char* out, size_t size) {
  HMODULE module = nullptr;
  if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                          reinterpret_cast<LPCWSTR>(address), &module)) {
    snprintf(out, size, "0x%llx", address);
    return;
  }
  char path[MAX_PATH] = {};
  GetModuleFileNameA(module, path, MAX_PATH);
  const char* name = strrchr(path, '\\');
  snprintf(out, size, "%s+0x%llx", name ? name + 1 : path, address - reinterpret_cast<DWORD64>(module));
}

void report() {
  log("hang: no frame for %d s; stacks of every thread follow (module+offset; the plugin's offsets map "
      "through build/Release/MetalGearSkate.map)", kStaleSeconds);
  std::vector<ThreadStack> stacks;
  if (HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0); snap != INVALID_HANDLE_VALUE) {
    THREADENTRY32 te{sizeof(te)};
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
      if (te.th32OwnerProcessID != GetCurrentProcessId() || te.th32ThreadID == GetCurrentThreadId()) continue;
      HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE,
                                 te.th32ThreadID);
      if (!thread) continue;
      ThreadStack stack;
      stack.id = te.th32ThreadID;
      if (SuspendThread(thread) != static_cast<DWORD>(-1)) {
        CONTEXT ctx{};
        ctx.ContextFlags = CONTEXT_FULL;
        if (GetThreadContext(thread, &ctx)) walk(ctx, stack);
        ResumeThread(thread);
      }
      CloseHandle(thread);
      stacks.push_back(stack);
    }
    CloseHandle(snap);
  }
  for (const ThreadStack& s : stacks) {
    log("hang: thread %lu", s.id);
    for (int i = 0; i < s.count; ++i) {
      char where[MAX_PATH + 32];
      describe(s.rip[i], where, sizeof(where));
      log("hang:   %s", where);
    }
  }
  const auto dump = g_dir / L"MetalGearSkate.hang.dmp";
  HANDLE file = CreateFileW(dump.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file != INVALID_HANDLE_VALUE) {
    const BOOL ok = MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), file,
                                      static_cast<MINIDUMP_TYPE>(MiniDumpWithThreadInfo), nullptr, nullptr, nullptr);
    CloseHandle(file);
    log("hang: minidump %s %ls", ok ? "written to" : "FAILED for", dump.c_str());
  }
}

DWORD WINAPI watch(LPVOID) {
  uint64_t last = 0;
  int stale = 0;
  for (;;) {
    Sleep(1000);
    const uint64_t presents = render::status().presents;
    if (presents != last) {
      last = presents;
      stale = 0;
    } else if (last > 0 && ++stale == kStaleSeconds) {
      report();  // once per hang; re-armed when frames come back
    }
  }
}

}  // namespace

void start(const std::filesystem::path& plugin_dir) {
  g_dir = plugin_dir;
  if (HANDLE thread = CreateThread(nullptr, 0, watch, nullptr, 0, nullptr)) CloseHandle(thread);
}

}  // namespace hang
