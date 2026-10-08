#pragma once

#include <cstdint>

// Poses Snake from the skater while skating.
//
// The skate thread publishes the retargeted pose (mgsk_snake_pose: 57 bone world matrices in
// the game's layout, rows X/Y/Z axis then translation, mm). A hook at the start of the game's
// render-object bone pass (exe+CE760 on MC 3.0.0, the function that copies an object's bone
// matrices at [obj+0xC0] for drawing) overwrites Snake's bones with it before they're used.
namespace pose {

constexpr int kSnakeBones = 57;

// Install the hook. Call once after game::init(). False if the signature wasn't found.
bool init();

// From the skate thread: the latest pose (kSnakeBones * 16 floats), or nullptr to stop posing.
void publish(const float* bones);

struct Stats {
  bool hooked = false;
  uint64_t calls = 0;     // hook calls seen
  uint64_t applied = 0;   // times Snake's bones were overwritten
  int last_count = 0;     // bone count of the last object seen
  float nearest_mm = -1;  // closest 57-bone object to Snake since the last call (-1 = none)
  int relative = -1;      // bones stored relative to the render origin (1) or world (0), -1 unknown
};
Stats stats();

}  // namespace pose
