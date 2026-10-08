#pragma once

#include "core_api.hpp"

#include <cstdint>
#include <string_view>

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

// Scans the game's code for the signatures below and installs the pad and stage hooks. Logs each
// result. Call once, after the game has unpacked (any time after our init thread starts).
// `shared` holds Snake's frozen state across core reloads.
bool init(core::CoreShared* shared);

// Removes the hooks (core unload). Snake stays frozen if he was (the loader owns that state).
// Put the original bytes back under every hook (trampolines kept; see hooks.hpp).
void disable_hooks();
void stop();

// Snake's gameplay position in mm (y up), or nullptr when no player exists (menus, loading).
// Writing it moves Snake for real; his feet IK keeps the old contact until he moves.
Vec3* snake_position();

// Snake's facing (yaw) as a 16-bit angle, 65536 = full turn, or nullptr when no player.
// Two copies, current and target; write both to turn him.
int16_t* snake_yaw();
int16_t* snake_target_yaw();

// Hand Snake's actor over while skating (true) or back to the game (false); idempotent. Frozen,
// his own per-frame update is skipped (no gravity, fall death, auto-run or input; we write his
// position) and his render object's bounds are made huge so it is never culled (it stays where
// he started; the renderer poses his mesh relative to it). False if his actor isn't laid out
// as expected (then nothing is changed).
bool freeze_snake(bool frozen);

// Snake's render object (his actor's, at a fixed offset from his control block), or nullptr.
const uint8_t* snake_render_object();

// True while his actor is frozen for skating (freeze_snake).
bool snake_frozen();

// The game camera's eye and look-at points (mm, y up), read from the static camera object at
// exe+1D39D80 (MC 3.0.0 only; no signature yet). False when they don't look like positions.
bool camera(Vec3& eye, Vec3& look_at);

// Name of the loaded stage, e.g. "s091a", or nullptr before init. Empty in menus.
const char* current_stage();

// Pad 0 as the game read it this frame, captured before any blanking. False before the first
// captured frame. Needs the pad hook (installed by init()).
bool pad(Pad& out);

// While true, the game's own view of pad 0 is blanked after each update (no buttons, sticks
// centred), so Snake's movement code ignores the controller. The real state still reaches pad().
void block_input(bool blocked);

// While on, harness-injected pad frames go to the skate engine (take_injected, one per engine
// tick) instead of the game's pad copy.
void engine_takes_injection(bool on);
// The next injected pad frame, if any (consumes it).
bool take_injected(Pad& out);

// Load a stage by code (e.g. "s091a", or "select" for the developer menu) the way MGSHDFix's
// "return to developer menu" does. Runs on the game thread at the next pad update. False if
// the functions weren't found or the name is too long.
bool request_stage(const char* name);

// Test harness: replace pad 0 with this state for the next `frames` game frames (before the
// plugin reads it and before blocking), as if a controller were held. 0 frames cancels.
void inject_pad(uint16_t buttons, uint8_t lx, uint8_t ly, uint8_t rx, uint8_t ry, int frames);
// Append a step after the currently queued ones (frame-exact sequences such as flick tricks).
void queue_pad(uint16_t buttons, uint8_t lx, uint8_t ly, uint8_t rx, uint8_t ry, int frames);
int injected_frames_left();

// The game's link-variable (save/state) buffer, or nullptr before init / outside gameplay.
// Field offsets follow MGSHDFix's mgs3_linkvarbuf.hpp.
uint8_t* linkvar();
constexpr size_t kLinkCurrentItem = 1494;      // short
constexpr int16_t kItemStealthCamouflage = 0x17;

// The unique match of an IDA-style signature ("48 8B ?? ...") in the game's image, or nullptr.
uint8_t* find(std::string_view signature);

}  // namespace game
