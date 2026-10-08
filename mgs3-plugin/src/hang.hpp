#pragma once

#include <filesystem>

// Hang watchdog: when the game stops presenting frames for several seconds, logs every thread's
// call stack (module+offset) and writes MetalGearSkate.hang.dmp beside the plugin, once per hang.
namespace hang {

void start(const std::filesystem::path& plugin_dir);

// Stops and joins the watchdog thread (core unload).
void stop();

}  // namespace hang
