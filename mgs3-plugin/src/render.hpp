#pragma once

#include <windows.h>

#include <cstdint>
#include <filesystem>
#include <string>

// Draws the skateboard into MGS3's own frame (Direct3D 11) while skating.
//
// Hooks IDXGISwapChain::Present and Present1 in dxgi.dll (addresses taken from a throwaway swap
// chain's vtable) and, just before each frame goes out, draws the seven board parts at the
// engine's pose (skate::board_pose, metres -> mm) in flat colours. That is after MGSHDFix's own
// post-processing, which runs at the game's Present call site. The view-projection is the one
// the game itself drew the frame's stage geometry with, read from its vertex-shader constants
// (Map/Unmap/UpdateSubresource hooks; see render.cpp). While skating with `camera skate` (the
// default) those constants are rewritten so the whole frame, board included, is drawn from the
// Skate 3 engine's chase camera (skate::camera). `camera object` falls back to the old guess: the
// camera object (game::camera) plus the [render] fov/near/far. All Direct3D state touched is saved and restored around the draw.
namespace render {

// Loads the board meshes (from [skate] skate_data) and installs the Present hooks.
void start(const std::filesystem::path& plugin_dir, HMODULE ffi);

// Removes the hooks and releases everything created on the game's device (core unload).
void stop();

// Test harness `render <key> <value>`: change a setting live (not saved to the ini).
// Keys: camera (skate|game|object), skate_fov_deg (0 = the engine's), fov_deg, near_mm, far_mm,
// aspect, flip_z, depth_game, debug (fov_deg..flip_z only matter for camera=object; aspect also
// overrides the back buffer's for the skate camera). `capture now` logs the last frame's captured
// matrices. An empty key just reports the current values. Returns "ok <values>" or "error: ...".
std::string set(const std::string& key, const std::string& value);

struct Status {
  bool hooked = false;
  uint64_t presents = 0;  // Present calls seen
  uint64_t drawn = 0;     // frames we drew into
  uint32_t width = 0, height = 0;  // last back buffer drawn into
};
Status status();

}  // namespace render
