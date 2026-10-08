// MetalGearSkate.asi: the loader. Loaded into METAL GEAR SOLID3.exe by an ASI loader; it loads
// skate_ffi.dll (the Skate 3 engine) once and then MetalGearSkate.core.dll (all of the plugin's
// game-side code), which it can unload and load again while the game runs: a new build is picked
// up without restarting the game (`python -I tools/gametest.py reload`).
//
// The core is loaded from a numbered copy (plugins\MetalGearSkate\live-<n>.dll), so the build can
// overwrite plugins\MetalGearSkate\core.dll while a core is running. A reload is requested by
// creating plugins\MetalGearSkate\reload (the harness does this) or by the core itself (its
// `reload` command sets CoreShared::reload_requested).

#include <windows.h>

#include <share.h>

#include <cstdarg>
#include <cstdio>
#include <filesystem>

#include <dbghelp.h>

#include "core_api.hpp"

#pragma comment(lib, "dbghelp.lib")

namespace {

HMODULE g_self = nullptr;
core::CoreShared g_shared;
std::filesystem::path g_dir;

// Snake's actor update while he skates: nothing (he is posed from the skate engine). Lives here,
// in the module that never unloads, because the game calls it through his actor's update slot.
void __fastcall frozen_update(void*) {}

void log_line(const char* fmt, ...) {
  FILE* file = _wfsopen((g_dir / L"MetalGearSkate.log").c_str(), L"a", _SH_DENYNO);
  if (!file) return;
  va_list args;
  va_start(args, fmt);
  fputs("loader: ", file);
  vfprintf(file, fmt, args);
  va_end(args);
  fputc('\n', file);
  fclose(file);
}

// Crash reports: a fatal exception anywhere in the game (access violation, heap corruption, a
// fail-fast) logs the crashing thread's stack (module+offset; the core's offsets map through
// build/Release/MetalGearSkate.core.map), writes plugins\MetalGearSkate.crash.dmp and keeps the
// session's log as MetalGearSkate.crash.log plus a timestamped copy (the next launch overwrites
// MetalGearSkate.log). Lives here so it covers core reloads too.
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

LONG g_crashing = 0;

void report_crash(EXCEPTION_POINTERS* info) {
  const DWORD code = info->ExceptionRecord->ExceptionCode;
  if (InterlockedExchange(&g_crashing, 1)) return;
  char where[MAX_PATH + 32];
  describe(reinterpret_cast<DWORD64>(info->ExceptionRecord->ExceptionAddress), where, sizeof(where));
  log_line("crash: exception 0x%08lX at %s (thread %lu); stack:", code, where, GetCurrentThreadId());
  CONTEXT ctx = *info->ContextRecord;
  for (int i = 0; i < 32 && ctx.Rip; ++i) {
    describe(ctx.Rip, where, sizeof(where));
    log_line("crash:   %s", where);
    DWORD64 image = 0;
    PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(ctx.Rip, &image, nullptr);
    if (!fn) {
      ctx.Rip = *reinterpret_cast<DWORD64*>(ctx.Rsp);
      ctx.Rsp += 8;
      continue;
    }
    void* handler_data = nullptr;
    DWORD64 frame = 0;
    RtlVirtualUnwind(UNW_FLAG_NHANDLER, image, ctx.Rip, fn, &ctx, &handler_data, &frame, nullptr);
  }
  const auto dump = g_dir / L"MetalGearSkate.crash.dmp";
  HANDLE file = CreateFileW(dump.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file != INVALID_HANDLE_VALUE) {
    MINIDUMP_EXCEPTION_INFORMATION mei{GetCurrentThreadId(), info, FALSE};
    MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), file,
                      static_cast<MINIDUMP_TYPE>(MiniDumpWithThreadInfo | MiniDumpWithIndirectlyReferencedMemory),
                      &mei, nullptr, nullptr);
    CloseHandle(file);
  }
  // The log (with the stack above) outlives the next launches.
  const auto log = g_dir / L"MetalGearSkate.log";
  CopyFileW(log.c_str(), (g_dir / L"MetalGearSkate.crash.log").c_str(), FALSE);
  SYSTEMTIME t;
  GetLocalTime(&t);
  wchar_t stamped[64];
  swprintf_s(stamped, L"MetalGearSkate.crash-%04u%02u%02u-%02u%02u%02u.log", t.wYear, t.wMonth, t.wDay, t.wHour,
             t.wMinute, t.wSecond);
  CopyFileW(log.c_str(), (g_dir / stamped).c_str(), FALSE);
  log_line("crash: log kept as MetalGearSkate.crash.log and %ls", stamped);
  InterlockedExchange(&g_crashing, 0);
}

// Heap corruption and fail-fast are fatal whoever handles them: report at first chance.
LONG CALLBACK on_fatal(EXCEPTION_POINTERS* info) {
  const DWORD code = info->ExceptionRecord->ExceptionCode;
  if (code == 0xC0000374 /* heap corruption */ || code == 0xC0000409 /* fail fast */) report_crash(info);
  return EXCEPTION_CONTINUE_SEARCH;
}

// Anything else (an access violation...) only when nothing handled it: the game and its DRM may
// raise and handle some on purpose.
LPTOP_LEVEL_EXCEPTION_FILTER g_previous_filter = nullptr;
LONG WINAPI on_unhandled(EXCEPTION_POINTERS* info) {
  report_crash(info);
  return g_previous_filter ? g_previous_filter(info) : EXCEPTION_CONTINUE_SEARCH;
}

struct Core {
  HMODULE module = nullptr;
  core::StopFn stop = nullptr;
  std::filesystem::path copy;
};

Core load_core(HMODULE ffi) {
  Core c;
  const auto folder = g_dir / L"MetalGearSkate";
  const auto master = folder / L"core.dll";
  std::error_code ec;
  if (!std::filesystem::exists(master, ec)) {
    log_line("no %ls; nothing to load", master.c_str());
    return c;
  }
  ++g_shared.generation;
  c.copy = folder / (L"live-" + std::to_wstring(g_shared.generation) + L".dll");
  // A previous game session may have left copies behind; they're unused now.
  for (const auto& entry : std::filesystem::directory_iterator(folder, ec)) {
    const auto name = entry.path().filename().wstring();
    if (name.starts_with(L"live-") && entry.path() != c.copy) std::filesystem::remove(entry.path(), ec);
  }
  if (!std::filesystem::copy_file(master, c.copy, std::filesystem::copy_options::overwrite_existing, ec)) {
    log_line("copying %ls failed (%s)", master.c_str(), ec.message().c_str());
    return c;
  }
  c.module = LoadLibraryW(c.copy.c_str());
  if (!c.module) {
    log_line("loading %ls failed (error %lu)", c.copy.c_str(), GetLastError());
    return c;
  }
  const auto start = reinterpret_cast<core::StartFn>(GetProcAddress(c.module, core::kStartExport));
  c.stop = reinterpret_cast<core::StopFn>(GetProcAddress(c.module, core::kStopExport));
  if (!start || !c.stop || !start(g_dir.c_str(), ffi, &g_shared)) {
    log_line("core %u didn't start", g_shared.generation);
    FreeLibrary(c.module);
    c.module = nullptr;
    c.stop = nullptr;
  }
  return c;
}

void unload_core(Core& c) {
  if (!c.module) return;
  if (c.stop) c.stop();
  FreeLibrary(c.module);
  std::error_code ec;
  std::filesystem::remove(c.copy, ec);
  c = {};
}

DWORD WINAPI run(LPVOID) {
  wchar_t path[MAX_PATH];
  GetModuleFileNameW(g_self, path, MAX_PATH);
  g_dir = std::filesystem::path(path).parent_path();
  g_shared.frozen_update = &frozen_update;
  AddVectoredExceptionHandler(0, on_fatal);
  g_previous_filter = SetUnhandledExceptionFilter(on_unhandled);

  const auto ffi_path = g_dir / L"skate_ffi.dll";
  HMODULE ffi = LoadLibraryW(ffi_path.c_str());
  if (!ffi) log_line("skate_ffi.dll not found next to the plugin (error %lu)", GetLastError());

  Core current = load_core(ffi);
  const auto trigger = g_dir / L"MetalGearSkate" / L"reload";
  for (;;) {
    Sleep(250);
    std::error_code ec;
    const bool file_request = std::filesystem::exists(trigger, ec);
    if (!file_request && !InterlockedExchange(&g_shared.reload_requested, 0)) continue;
    if (file_request) std::filesystem::remove(trigger, ec);
    const DWORD t0 = GetTickCount();
    unload_core(current);
    current = load_core(ffi);
    log_line("reloaded core %u in %lu ms", g_shared.generation, GetTickCount() - t0);
  }
}

}  // namespace

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
  if (reason == DLL_PROCESS_ATTACH) {
    // The ASI loader also runs in launcher.exe; only the game itself matters.
    wchar_t host[MAX_PATH];
    GetModuleFileNameW(nullptr, host, MAX_PATH);
    if (_wcsicmp(std::filesystem::path(host).filename().c_str(), L"METAL GEAR SOLID3.exe") != 0) return TRUE;
    g_self = instance;
    DisableThreadLibraryCalls(instance);
    // Never do real work under the loader lock.
    if (HANDLE thread = CreateThread(nullptr, 0, run, nullptr, 0, nullptr)) CloseHandle(thread);
  }
  return TRUE;
}
