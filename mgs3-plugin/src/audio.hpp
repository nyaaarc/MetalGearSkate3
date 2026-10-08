#pragma once

#include <windows.h>

#include <cstdint>

#include "skate_ffi.h"

// The skater's sound: plays the Skate 3 audio runtime's output (skate_ffi.dll's
// mgsk_audio_render, 48 kHz stereo) through the default output device (WASAPI, shared mode) on
// its own thread. The engine side (posts, MixMap, rolling bed) runs in the DLL with each engine
// tick; this module only moves the rendered samples to the device.
namespace audio {

// Open the device and start the output thread. `volume` (0..kMaxVolume) scales the output; above
// 1 the engine's (quiet) mix is boosted, with a soft limiter. False if the
// DLL has no audio exports or no output device opened (logged; skating works without sound).
constexpr float kMaxVolume = 4.f;
bool start(HMODULE ffi, float volume);

// Change the volume live (0..kMaxVolume).
void set_volume(float volume);
float volume();

// The session whose sound plays (null: silence). Starts its sound in the DLL first if needed,
// from the converted data's `assets` folder; false if that failed (logged).
bool use_session(MgskSession* session, const char* assets_dir);

// Sound only while skating (the runtime holds its last voices otherwise).
void set_playing(bool playing);

// Stop the thread and close the device (core unload).
void stop();

struct Status {
  bool running = false;
  uint64_t frames = 0;      // frames written to the device
  uint64_t underruns = 0;   // times the device's buffer had run dry before a write
};
Status status();

}  // namespace audio
