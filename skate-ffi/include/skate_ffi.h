/* C API over the Skate 3 Rust engine. Mirrors skate-ffi/src/lib.rs.
 * Functions returning int give 0 on success and -1 on failure; see mgsk_last_error. */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct MgskSession MgskSession;

/* Controller state in XInput's layout. */
typedef struct MgskControls {
  uint16_t buttons;
  uint8_t left_trigger;
  uint8_t right_trigger;
  int16_t left[2];
  int16_t right[2];
} MgskControls;

typedef char mgsk_controls_size_check[sizeof(MgskControls) == 12 ? 1 : -1];

/* Static string identifying this build. Never null. */
const char* mgsk_version(void);
/* Message for the last failure on this thread. Never null; empty if none. */
const char* mgsk_last_error(void);

/* Loads converted Skate 3 data from assets_dir and builds a world from triangle_count
 * triangles (9 floats each). spawn is 3 floats. Null on failure. */
MgskSession* mgsk_session_new(const char* assets_dir, const float* triangles, uint32_t triangle_count,
                              const float* spawn, float heading);
void mgsk_session_free(MgskSession* session);
/* Replaces the world the skater rides (9 floats per triangle); takes effect next tick. */
int mgsk_install_world(MgskSession* session, const float* triangles, uint32_t triangle_count);
/* Replaces the world with the solid collision of an MGS3 .geom file, coordinates (mm) times
 * units_per_mm; takes effect next tick. Returns the triangle count, or -1 on failure. */
int mgsk_install_geom(MgskSession* session, const char* geom_path, float units_per_mm);
/* Number of solid triangles in a .geom file, or -1 on failure. Decodes only. */
int mgsk_geom_triangle_count(const char* geom_path);
/* Writes up to capacity .geom triangles (9 floats each, mm) to out; returns the total, or -1. */
int mgsk_geom_triangles(const char* geom_path, float* out, uint32_t capacity);
/* Seconds per engine tick, or -1 on failure. */
float mgsk_period(const MgskSession* session);
/* Puts the skater on the board at position (3 floats) facing heading radians. */
int mgsk_activate(MgskSession* session, const float* position, float heading);
/* Advances one engine tick with this controller state. */
int mgsk_tick(MgskSession* session, const MgskControls* controls);
/* Writes the skater root transform (16 floats, column-major). */
int mgsk_root(const MgskSession* session, float* out);

#ifdef __cplusplus
}
#endif
