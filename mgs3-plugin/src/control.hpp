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
//   seq <step> ; <step> ; ...                    play pad states back to back, frame-exact
//                                                (each step: buttons lx ly rx ry frames)
//   teleport <x> <y> <z>                         move Snake (mm, his waist position; floor + 800)
//   block on|off                                 blank Snake's pad input without skating
//   peek <offset> <i8|i16|i32|f32>              read the link-variable (state) buffer
//   poke <offset> <type> <value>                 write it (debugging only)
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
