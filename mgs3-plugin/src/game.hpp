#pragma once

#include <cstdint>

// What the plugin knows about MGS3's memory, found by signature scan at startup.
// Layouts and addresses are documented in docs/formats.md ("Runtime findings").
namespace game {

struct Vec3 {
  float x, y, z;
};

// One controller as the game sees it, after Steam Input: an emulated DualShock 2.
// Buttons are PS2 bits, active high (Cross 0x4000, Circle 0x2000, Square 0x8000, Triangle 0x1000,
// L1 0x0400, R1 0x0800, L2 0x0100, R2 0x0200, Start 0x0008, Select 0x0001, L3 0x0002, R3 0x0004,
// Up 0x0010, Right 0x0020, Down 0x0040, Left 0x0080). Verified: Cross, left stick Y. The rest is
// the standard PS2 layout, assumed. Sticks are 0..255, centre 128, up = 0.
struct Pad {
  int32_t status;  // -1 seen while connected
  uint16_t buttons;
  uint16_t unknown;
  uint8_t right_x, right_y, left_x, left_y;
};

// Scans the game's code for the signatures below. Logs each result. Call once, after the
// game has unpacked (any time after our init thread starts).
bool init();

// Snake's gameplay position in mm (y up), or nullptr when no player exists (menus, loading).
// Writing it moves Snake for real; his feet IK keeps the old contact until he moves.
Vec3* snake_position();

// Snake's facing (yaw) as a 16-bit angle, 65536 = full turn, or nullptr when no player.
// Two copies, current and target; write both to turn him.
int16_t* snake_yaw();
int16_t* snake_target_yaw();

// Name of the loaded stage, e.g. "s091a", or nullptr before init. Empty in menus.
const char* current_stage();

// The game's processed state for pad 0 (what gameplay reads), or nullptr before init.
const Pad* pad();

}  // namespace game
