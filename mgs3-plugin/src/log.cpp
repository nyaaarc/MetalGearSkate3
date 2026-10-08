#include "log.hpp"

#include <share.h>

#include <cstdarg>
#include <atomic>
#include <cstdio>
#include <mutex>

namespace {
FILE* g_file = nullptr;
std::mutex g_mutex;
std::atomic<bool> g_verbose{false};
}  // namespace

void log_open(const std::filesystem::path& path, bool append) {
  std::lock_guard lock(g_mutex);
  if (!append) {
    // Keep the previous session's log (e.g. one that ended in a hang) as <name>.prev.log.
    std::error_code ignored;
    std::filesystem::copy_file(path, std::filesystem::path(path).replace_extension(L".prev.log"),
                               std::filesystem::copy_options::overwrite_existing, ignored);
  }
  // Shared so the log can be read while the game runs (and the loader can append to it).
  g_file = _wfsopen(path.c_str(), append ? L"a" : L"w", _SH_DENYNO);
}

void log_close() {
  std::lock_guard lock(g_mutex);
  if (g_file) fclose(g_file);
  g_file = nullptr;
}

void set_log_verbose(bool on) { g_verbose = on; }

bool log_verbose() { return g_verbose; }

void log(const char* fmt, ...) {
  std::lock_guard lock(g_mutex);
  if (!g_file) return;
  va_list args;
  va_start(args, fmt);
  vfprintf(g_file, fmt, args);
  va_end(args);
  fputc('\n', g_file);
  fflush(g_file);
}
