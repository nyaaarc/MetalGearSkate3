#include "log.hpp"

#include <share.h>

#include <cstdarg>
#include <cstdio>
#include <mutex>

namespace {
FILE* g_file = nullptr;
std::mutex g_mutex;
}  // namespace

void log_open(const std::filesystem::path& path) {
  std::lock_guard lock(g_mutex);
  // Shared so the log can be read while the game runs.
  g_file = _wfsopen(path.c_str(), L"w", _SH_DENYNO);
}

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
