#pragma once

#include <cstdint>

// Poses Snake from the skater while skating.
//
// The skate thread publishes the retargeted pose (mgsk_snake_pose: 57 bone world matrices in
// the game's layout, rows X/Y/Z axis then translation, mm). The GPU skins Snake from a bone
// palette in the vertex-shader constants, not from the render object's bone array, so the
// renderer (render.cpp) rewrites his palette with this pose as the game uploads it. This module
// holds the published pose and, from a hook at the start of the game's render-object bone pass
// (exe+CE760 on MC 3.0.0), Snake's own bones for the frame, which the renderer needs to tell
// which palette slot is which bone.
namespace pose {

constexpr int kSnakeBones = 57;

// Install the hook. Call once after game::init(). False if the signature wasn't found.
bool init();

// From the skate thread: the latest pose (kSnakeBones * 16 floats), or nullptr to stop posing.
void publish(const float* bones);

// The published pose, if any (kSnakeBones * 16 floats).
bool current(float* bones);

// Snake's bones as the game computed them on the last bone pass (kSnakeBones * 16 floats, game
// layout, world mm), the render origin of the pass, the render object's world matrix (obj+0x20;
// its translation is the skinning origin) and whether the bones were stored relative to the
// origin. False before Snake is seen.
bool game_bones(float* bones, float origin[3], float object[16], bool& relative);

// From the renderer: palette slots rewritten with the pose / Snake draws seen this frame.
void count_applied(uint64_t slots, uint64_t draws);

struct Stats {
  bool hooked = false;
  uint64_t calls = 0;     // hook calls seen
  uint64_t applied = 0;   // palette slots rewritten (renderer)
  int last_count = 0;     // bone count of the last object seen
  float nearest_mm = -1;  // closest 57-bone object to Snake since the last call (-1 = none)
  int relative = -1;      // bones stored relative to the render origin (1) or world (0), -1 unknown
  uint64_t draws = 0;     // Snake skinned draws seen by the renderer
};
Stats stats();

}  // namespace pose
