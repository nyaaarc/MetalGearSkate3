#pragma once

#include <windows.h>

#include <filesystem>

// Skate mode: runs the Skate 3 engine (skate_ffi.dll) and drives Snake from it.
//
// First-test version: F5 toggles skating where Snake stands, on a flat stand-in floor at his
// feet (good enough for flat stages such as Groznyj Grad, s091a). The engine ticks on its own
// thread at its fixed period, reads the first XInput pad, and writes the skater's position
// into Snake's gameplay position. Settings come from MetalGearSkate.ini beside the plugin.
namespace skate {

void start(const std::filesystem::path& plugin_dir, HMODULE ffi);

}  // namespace skate
