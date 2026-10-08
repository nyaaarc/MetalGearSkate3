#pragma once

#include <filesystem>

// Plain-text log beside the plugin, readable while the game runs. Thread-safe.
// `append` continues the file (a core reload) instead of starting it again.
void log_open(const std::filesystem::path& path, bool append = false);
void log_close();
void log(const char* fmt, ...);

// Periodic diagnostics (camera captures, pose counters, positions) are logged only while verbose
// (`render debug on`), so the log stays readable.
void set_log_verbose(bool on);
bool log_verbose();
