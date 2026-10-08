#pragma once

#include <cstdint>

#include "skate.hpp"

// Puts MGS3's own main camera on the skate engine's chase camera while skating, so the game
// draws and culls the scene for that view itself (re-projecting the game's draws can't bring
// back scenery the game culled for its own camera).
namespace camera {

// Install the hook. Call once after game::init(). False if the signatures weren't found.
bool init();

// Removes the hook (core unload).
// Put the original bytes back under the hook (trampoline kept; see hooks.hpp).
void disable_hooks();
void stop();

// Drive the game camera from the skate camera (only has an effect while skating).
void set_driving(bool on);
bool driving();

// The game camera's zoom factor while driving (its +0x9C); 0 (default) matches the skate
// camera's field of view.
void set_zoom(float zoom);

// Orbit camera for checks: on a circle around the skater (yaw relative to his heading, 0 = in
// front looking back, 90 = his right, 180 = behind; distance and height in mm), looking at him.
// Off: the chase camera again. Only while driving.
void set_orbit(bool on, float yaw_deg = 90.f, float dist_mm = 2500.f, float height_mm = 1200.f);
bool orbiting();

// The board and Snake's pose from the recent camera placement whose eye is the frame's camera
// reference `L` (the GPU draws a frame with the camera from one to three updates back), with its
// tick counter and |eye - L|, so a frame poses Snake in step with the camera it is drawn with.
// False when not driving, or `coherent` is off.
bool frame_pose_at(const float (&L)[3], float (&board)[skate::kBoardBodies * 16], float (&snake)[57 * 16],
                   uint64_t& tick, float& error_mm);
void set_coherent(bool on);  // `render coherent 0|1` (default on)

// Forget the stored placements (skate off): without this the renderer kept posing Snake (and the
// board) from the last ride's placements after skate off, drawing his body at the old spot.
void clear_poses();
bool coherent();
// The pose tick the game camera was last placed from (0 = none), for diagnostics.
uint64_t camera_tick();
// The game camera eye we last wrote (mm), for diagnostics. False before any.
bool last_eye(float (&eye)[3]);
// After the last driven update: the camera's working eye/target as the game left them, and the
// eye/target we wrote (mm). Diagnostics.
bool camera_after(float (&eye)[3], float (&target)[3], float (&wrote_eye)[3], float (&wrote_target)[3]);

// Camera updates overridden so far (diagnostics).
uint64_t driven();

}  // namespace camera
