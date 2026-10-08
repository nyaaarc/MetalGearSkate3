#pragma once

#include <filesystem>

// Plain-text log beside the plugin, readable while the game runs. Thread-safe.
void log_open(const std::filesystem::path& path);
void log(const char* fmt, ...);
