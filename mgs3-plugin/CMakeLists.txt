cmake_minimum_required(VERSION 3.24)
project(MetalGearSkate CXX)

# MGS3 Master Collection is a 64-bit game; build with an x64 toolchain.
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR "Build mgs3-plugin for x64 (METAL GEAR SOLID3.exe is 64-bit).")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Bake the C runtime in so players don't need the VC++ redistributable.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# Function hooks (BSL-1.0, the library MGSHDFix uses), with its Zydis disassembler.
include(FetchContent)
set(SAFETYHOOK_FETCH_ZYDIS ON CACHE BOOL "" FORCE)
FetchContent_Declare(safetyhook GIT_REPOSITORY https://github.com/cursey/safetyhook.git GIT_TAG v0.7.0)
FetchContent_MakeAvailable(safetyhook)

# The loader: MetalGearSkate.asi, picked up by Ultimate ASI Loader. Tiny and never unloaded; it
# loads skate_ffi.dll once and the core below, which it can swap while the game runs.
add_library(MetalGearSkate SHARED src/loader.cpp)
set_target_properties(MetalGearSkate PROPERTIES SUFFIX ".asi")
target_compile_definitions(MetalGearSkate PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
target_compile_options(MetalGearSkate PRIVATE /W4 /permissive-)

# The core: everything else, as MetalGearSkate.core.dll (installed as plugins\MetalGearSkate\core.dll).
add_library(MetalGearSkateCore SHARED src/core.cpp src/camera.cpp src/control.cpp src/game.cpp src/hang.cpp
            src/log.cpp src/pose.cpp src/render.cpp src/skate.cpp src/watch.cpp)
set_target_properties(MetalGearSkateCore PROPERTIES OUTPUT_NAME "MetalGearSkate.core")
# The C API the skate engine DLL exports.
target_include_directories(MetalGearSkateCore PRIVATE ../skate-ffi/include)
target_compile_definitions(MetalGearSkateCore PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
target_compile_options(MetalGearSkateCore PRIVATE /W4 /permissive-)
target_link_libraries(MetalGearSkateCore PRIVATE safetyhook::safetyhook)
# A map file, to turn the hang watchdog's MetalGearSkate.core.dll+offset frames into functions.
target_link_options(MetalGearSkateCore PRIVATE /MAP)
