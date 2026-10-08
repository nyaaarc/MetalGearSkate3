#pragma once

#include <filesystem>

// Reverse-engineering aid: a hardware write breakpoint driven by a file.
//
// Put a hex address in `<dir>/MetalGearSkate.watch` and every distinct instruction that
// writes those 4 bytes is logged once, with registers and nearby code bytes (enough to
// build a signature). Delete the file to disarm. Polls twice a second.
void watch_start(const std::filesystem::path& dir);
