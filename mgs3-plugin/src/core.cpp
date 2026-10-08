// MetalGearSkate.core.dll: the game side of Metal Gear Skate, loaded by MetalGearSkate.asi (the
// loader, loader.cpp), which can unload and load it again while the game runs. mgsk_core_start
// finds Snake by signature (game.hpp), hooks his posing (pose.hpp) and the camera (camera.hpp),
// starts skate mode (skate.hpp, F5), draws the board (render.hpp) and runs the harness command
// file (control.hpp) and the hang watchdog (hang.hpp). mgsk_core_stop undoes all of it: every
// hook removed, every thread joined, every change to the game put back, so the module can be
// freed. State that should outlive a reload (the engine session) is handed to the loader
// (core_api.hpp).

#include <windows.h>
#include <psapi.h>

#include <atomic>
#include <filesystem>

#include "camera.hpp"
#include "control.hpp"
#include "core.hpp"
#include "game.hpp"
#include "hang.hpp"
#include "log.hpp"
#include "pose.hpp"
#include "render.hpp"
#include "skate.hpp"
#include "watch.hpp"

#pragma comment(lib, "psapi.lib")

namespace {

core::CoreShared* g_shared = nullptr;
std::filesystem::path g_dir;
HMODULE g_ffi = nullptr;
std::atomic<bool> g_stop{false};
HANDLE g_init_thread = nullptr;
// Which parts started (stop undoes only those).
bool g_started_modules = false;

void log_module(const wchar_t* name) {
  HMODULE module = GetModuleHandleW(name);
  MODULEINFO info{};
  if (module && GetModuleInformation(GetCurrentProcess(), module, &info, sizeof(info))) {
    log("%ls base=%p size=0x%lx", name ? name : L"<exe>", info.lpBaseOfDll, info.SizeOfImage);
  } else {
    log("%ls not loaded", name ? name : L"<exe>");
  }
}

// Sleeps up to `ms`, returning early (false) when the core is stopping.
bool nap(DWORD ms) {
  for (DWORD left = ms; left > 0;) {
    if (g_stop) return false;
    const DWORD step = left < 100 ? left : 100;
    Sleep(step);
    left -= step;
  }
  return !g_stop;
}

DWORD WINAPI init(LPVOID) {
  if (g_shared->generation == 1) {
    log_module(nullptr);
    log_module(L"Engine.dll");
    log_module(L"Renderer.dll");
  }
  watch_start(g_dir);
  if (!g_ffi) log("skate_ffi.dll isn't loaded; skating is unavailable");
  using VersionFn = const char*(__cdecl*)();
  auto version = g_ffi ? reinterpret_cast<VersionFn>(GetProcAddress(g_ffi, "mgsk_version")) : nullptr;
  if (version) log("skate_ffi.dll: %s", version());

  // SteamStub unpacks the game's code after DLLs load, so the scan can fail at first.
  bool found = false;
  for (int attempt = 0; attempt < 120 && !(found = game::init(g_shared)); ++attempt) {
    if (!nap(1000)) return 0;
  }
  if (!found) return 0;
  pose::init();
  camera::init();
  if (g_ffi) skate::start(g_dir, g_ffi, g_shared);
  render::start(g_dir, g_ffi);
  control::start(g_dir);
  hang::start(g_dir);
  g_started_modules = true;
  log("core %u ready", g_shared->generation);

  // Log Snake's position whenever it moves by more than half a metre (verbose only).
  game::Vec3 last{};
  bool had_player = false;
  while (nap(2000)) {
    const game::Vec3* p = game::snake_position();
    if (!p) {
      if (had_player && log_verbose()) log("snake: no player");
      had_player = false;
      continue;
    }
    const float dx = p->x - last.x, dz = p->z - last.z;
    if (!had_player || dx * dx + dz * dz > 500.f * 500.f) {
      if (log_verbose()) log("snake: position %.1f %.1f %.1f", p->x, p->y, p->z);
      last = *p;
    }
    had_player = true;
  }
  return 0;
}

}  // namespace

namespace core {

CoreShared* shared() { return g_shared; }

void request_reload() {
  if (g_shared) InterlockedExchange(&g_shared->reload_requested, 1);
}

}  // namespace core

extern "C" __declspec(dllexport) bool __cdecl mgsk_core_start(const wchar_t* plugin_dir, HMODULE ffi,
                                                              core::CoreShared* shared) {
  if (!shared || shared->version != core::kSharedVersion) return false;
  g_shared = shared;
  g_dir = plugin_dir;
  g_ffi = ffi;
  g_stop = false;
  log_open(g_dir / L"MetalGearSkate.log", shared->generation > 1);
  log("MetalGearSkate core %u loaded", shared->generation);
  g_init_thread = CreateThread(nullptr, 0, init, nullptr, 0, nullptr);
  return g_init_thread != nullptr;
}

extern "C" __declspec(dllexport) void __cdecl mgsk_core_stop() {
  const DWORD t0 = GetTickCount();
  g_stop = true;
  if (g_init_thread) {
    WaitForSingleObject(g_init_thread, INFINITE);
    CloseHandle(g_init_thread);
    g_init_thread = nullptr;
  }
  if (g_started_modules) {
    // Users of the hooks first, then the hooks; the skate thread hands its session over.
    control::stop();
    hang::stop();
    skate::stop();
    render::stop();
    camera::stop();
    pose::stop();
  }
  game::stop();
  watch_stop();
  // Let any game thread still running in one of our (now removed) hooks finish before the
  // module goes away.
  Sleep(250);
  log("core %u stopped in %lu ms", g_shared ? g_shared->generation : 0, GetTickCount() - t0);
  log_close();
  g_started_modules = false;
}
