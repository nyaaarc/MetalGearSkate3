#pragma once

#include <windows.h>

#include <filesystem>

// Skate mode: runs the Skate 3 engine (skate_ffi.dll) and drives Snake from it.
//
// F5 (or request()) toggles skating where Snake stands, on the stage's own collision (flat floor
// fallback). The engine ticks on its own thread at its fixed period, reads the game's pad 0
// (XInput first if an Xbox pad is connected), blocks Snake's own input, and writes the skater's
// position and heading into Snake. Settings come from MetalGearSkate.ini beside the plugin.
namespace skate {

void start(const std::filesystem::path& plugin_dir, HMODULE ffi);

// Test harness: ask the skate thread to start (true) or stop (false) skating, as F5 would.
void request(bool on);

struct Status {
  bool ready = false;    // engine loaded
  bool skating = false;
  float skater[3] = {};  // engine units (m)
  float heading_deg = 0;
  int collision_triangles = -1;  // -1 = flat floor / none
};
Status status();

// The board's body transforms from the latest engine tick: MGSK_BODY_COUNT column-major 4x4
// matrices in engine units (m), MGSK_BODY_* order. False while not skating (or when the DLL
// has no mgsk_board_pose). Safe from any thread; the renderer reads it every frame.
constexpr int kBoardBodies = 7;
bool board_pose(float (&out)[kBoardBodies * 16]);

}  // namespace skate
