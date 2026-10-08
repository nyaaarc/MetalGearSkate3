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

// Pad 0 as the game read it this frame, captured before any blanking. False before the first
// captured frame. Needs the pad hook (installed by init()).
bool pad(Pad& out);

// While true, the game's own view of pad 0 is blanked after each update (no buttons, sticks
// centred), so Snake's movement code ignores the controller. The real state still reaches pad().
void block_input(bool blocked);

// Test harness: replace pad 0 with this state for the next `frames` game frames (before the
// plugin reads it and before blocking), as if a controller were held. 0 frames cancels.
void inject_pad(uint16_t buttons, uint8_t lx, uint8_t ly, uint8_t rx, uint8_t ry, int frames);
int injected_frames_left();

}  // namespace game
