// MetalGearSkate.asi: the game side. Loaded into METAL GEAR SOLID3.exe by an ASI loader,
// it loads skate_ffi.dll (the Skate 3 engine) from the same folder.
//
// So far: log the game module layout, load the skate engine DLL, find Snake by signature
// (game.hpp) and log his position, start skate mode (skate.hpp, F5), draw the board (render.hpp), and run the file-driven write watch (watch.hpp) used for
// reverse engineering. No gameplay hooks yet.

#include <windows.h>
#include <psapi.h>

#include <filesystem>

#include "camera.hpp"
#include "control.hpp"
#include "hang.hpp"
#include "game.hpp"
#include "log.hpp"
#include "pose.hpp"
#include "render.hpp"
#include "skate.hpp"
#include "watch.hpp"

#pragma comment(lib, "psapi.lib")

namespace {

HMODULE g_self = nullptr;

std::filesystem::path module_dir(HMODULE module) {
  wchar_t path[MAX_PATH];
  GetModuleFileNameW(module, path, MAX_PATH);
  return std::filesystem::path(path).parent_path();
}

void log_module(const wchar_t* name) {
  HMODULE module = GetModuleHandleW(name);
  MODULEINFO info{};
  if (module && GetModuleInformation(GetCurrentProcess(), module, &info, sizeof(info))) {
    log("%ls base=%p size=0x%lx", name ? name : L"<exe>", info.lpBaseOfDll, info.SizeOfImage);
  } else {
    log("%ls not loaded", name ? name : L"<exe>");
  }
}

DWORD WINAPI init(LPVOID) {
  const auto dir = module_dir(g_self);
  log_open(dir / L"MetalGearSkate.log");
  log("MetalGearSkate loaded");
  log_module(nullptr);
  log_module(L"Engine.dll");
  log_module(L"Renderer.dll");
  watch_start(dir);

  const auto ffi_path = dir / L"skate_ffi.dll";
  HMODULE ffi = LoadLibraryW(ffi_path.c_str());
  if (!ffi) {
    log("skate_ffi.dll not found next to the game (error %lu)", GetLastError());
    return 0;
  }
  using VersionFn = const char*(__cdecl*)();
  auto version = reinterpret_cast<VersionFn>(GetProcAddress(ffi, "mgsk_version"));
  log("skate_ffi.dll loaded: %s", version ? version() : "<no mgsk_version export>");

  // SteamStub unpacks the game's code after DLLs load, so the scan can fail at first.
  bool found = false;
  for (int attempt = 0; attempt < 120 && !(found = game::init()); ++attempt) Sleep(1000);
  if (!found) return 0;
  pose::init();
  camera::init();
  skate::start(dir, ffi);
  render::start(dir, ffi);
  control::start(dir);
  hang::start(dir);

  // Log Snake's position whenever it moves by more than half a metre (to verify the lookup).
  game::Vec3 last{};
  bool had_player = false;
  for (;;) {
    Sleep(2000);
    const game::Vec3* p = game::snake_position();
    if (!p) {
      if (had_player) log("snake: no player");
      had_player = false;
      continue;
    }
    const float dx = p->x - last.x, dz = p->z - last.z;
    if (!had_player || dx * dx + dz * dz > 500.f * 500.f) {
      log("snake: position %.1f %.1f %.1f", p->x, p->y, p->z);
      last = *p;
    }
    had_player = true;
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
    if (HANDLE thread = CreateThread(nullptr, 0, init, nullptr, 0, nullptr)) CloseHandle(thread);
  }
  return TRUE;
}
