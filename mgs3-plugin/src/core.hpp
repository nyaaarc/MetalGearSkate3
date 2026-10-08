#pragma once

#include "core_api.hpp"

// The running core's view of the loader-owned state (core_api.hpp).
namespace core {

// Null before mgsk_core_start.
CoreShared* shared();

// Ask the loader to unload this core and load the current build (takes effect within ~250 ms).
void request_reload();

}  // namespace core
