#pragma once

#include <filesystem>

// Test-harness control channel (used by tools/gametest.py; see docs/TESTING.md).
//
// Every 50 ms: if `<dir>/MetalGearSkate.cmd` exists, run each line, append the results to
// `<dir>/MetalGearSkate.cmdlog`, and delete the command file. Every 200 ms: write the game and
// skate state to `<dir>/MetalGearSkate.state.json`.
//
// Commands (one per line, # comments allowed):
//   pad <buttons> <lx> <ly> <rx> <ry> <frames>   hold a virtual pad state (PS2 button names
//                                                joined with +, or a hex mask; sticks 0..255,
//                                                128 centre, up = 0)
//   release                                      cancel any held virtual pad state
//   skate on | skate off                         same as pressing F5
//   stage <code>                                 load a stage directly (e.g. s091a; "select" =
//                                                developer menu)
//   mark <text>                                  write <text> to the plugin log
//   render                                       report the board renderer's settings
//   render <key> <value>                         change one live (not saved): fov_deg,
//                                                near_mm, far_mm, aspect (0 = back buffer),
//                                                flip_z, depth_game, debug (0/1 or on/off;
//                                                debug draws a marker and axes at Snake)
namespace control {

void start(const std::filesystem::path& plugin_dir);

}  // namespace control
