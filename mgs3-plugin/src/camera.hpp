#pragma once

#include <cstdint>

// Puts MGS3's own main camera on the skate engine's chase camera while skating, so the game
// draws and culls the scene for that view itself (re-projecting the game's draws can't bring
// back scenery the game culled for its own camera).
namespace camera {

// Install the hook. Call once after game::init(). False if the signatures weren't found.
bool init();

// Drive the game camera from the skate camera (only has an effect while skating).
void set_driving(bool on);
bool driving();

// Camera updates overridden so far (diagnostics).
uint64_t driven();

}  // namespace camera
