#pragma once

// The interface between MetalGearSkate.asi (the loader, which never unloads) and
// MetalGearSkate.core.dll (everything else, which the loader can unload and load again while the
// game runs). State that must survive a reload lives in CoreShared, owned by the loader.

#include <windows.h>

#include <cstdint>

namespace core {

constexpr uint32_t kSharedVersion = 3;

// Snake's actor while frozen for skating (game::freeze_snake): the slot points at the loader's
// stub, so a core can be unloaded with him frozen and the next core can thaw him.
struct Frozen {
  uint8_t* control = nullptr;
  void* update = nullptr;     // the actor's own update, put back on unfreeze
  uint8_t bounds[0x30] = {};  // render object bounds +0x120..+0x14F as they were
};

struct CoreShared {
  uint32_t version = kSharedVersion;
  uint32_t generation = 0;  // which core load this is (1 = first)
  // Engine sessions (MgskSession*), kept so a reload doesn't spend seconds loading one again.
  void* session = nullptr;
  void* spare = nullptr;
  bool posing = false;  // the session has Snake's retargeting
  char collision_stage[64] = {};  // stage whose collision `session` holds ("" = flat floor)
  int collision_triangles = -1;
  int rails = -1;
  // Skating at unload: the next core puts him back on the board here (Snake's waist, mm; MGS3
  // heading in degrees), so a reload mid-ride only costs a moment.
  bool resume = false;
  float resume_mm[3] = {};
  float resume_heading_deg = 0;
  // Where Snake stood when he got on the board (mm): solid ground of the game's own, where skate
  // off puts him back if the game starts dropping him.
  float entry_mm[3] = {};
  Frozen frozen;
  void(__fastcall* frozen_update)(void*) = nullptr;  // the loader's no-op actor update
  volatile LONG reload_requested = 0;  // set by the core (harness `reload`); the loader acts on it
};

// Exported by the core.
using StartFn = bool(__cdecl*)(const wchar_t* plugin_dir, HMODULE ffi, CoreShared* shared);
using StopFn = void(__cdecl*)();
constexpr const char* kStartExport = "mgsk_core_start";
constexpr const char* kStopExport = "mgsk_core_stop";

}  // namespace core
