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

/* Engine board bodies, the order of mgsk_board_pose and the part of mgsk_board_mesh. */
enum {
  MGSK_BODY_RIGHT_FRONT_WHEEL = 0,
  MGSK_BODY_LEFT_FRONT_WHEEL = 1,
  MGSK_BODY_RIGHT_BACK_WHEEL = 2,
  MGSK_BODY_LEFT_BACK_WHEEL = 3,
  MGSK_BODY_FRONT_TRUCK = 4,
  MGSK_BODY_BACK_TRUCK = 5,
  MGSK_BODY_DECK = 6,
  MGSK_BODY_COUNT = 7
};

/* One skateboard mesh vertex, in its body's local frame (metres). uv is glTF's (0,0 top left). */
typedef struct MgskBoardVertex {
  float position[3];
  float normal[3];
  float uv[2];
} MgskBoardVertex;

typedef char mgsk_board_vertex_size_check[sizeof(MgskBoardVertex) == 32 ? 1 : -1];

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
 * units_per_mm, plus grind rails found on its ledges, kerbs and railings; takes effect next
 * tick. Returns the triangle count, or -1 on failure. */
int mgsk_install_geom(MgskSession* session, const char* geom_path, float units_per_mm);
/* Grind rails the last mgsk_install_geom gave the session (0 before any), or -1. */
int mgsk_rail_count(const MgskSession* session);
/* The rails mgsk_install_geom would find in a .geom file, as segments (6 floats: start, end,
 * coordinates as mgsk_install_geom); writes up to capacity, returns the total segment count,
 * or -1. rail_count (may be null) receives the number of rails. For tests and debugging. */
int mgsk_geom_rails(const char* geom_path, float units_per_mm, float* out, uint32_t capacity, int* rail_count);
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
/* Number of skater bones (36 with the stock skater: body, board and *_REPARENTED helpers), or -1. */
int mgsk_bone_count(const MgskSession* session);
/* Copies bone `index`'s name (e.g. "LEFTFOREARM"), NUL-terminated and truncated to fit, into out
 * (capacity bytes; out may be null when capacity is 0). Returns the full name length, or -1. */
int mgsk_bone_name(const MgskSession* session, uint32_t index, char* out, uint32_t capacity);
/* Writes up to max_bones skater bone transforms (16 floats each, column-major) to out, in
 * mgsk_bone_name order. Root-relative, not world: bone i in world = root * bone[i], root from
 * mgsk_root. Metres, engine axes. Returns the count written, or -1. */
int mgsk_bones(const MgskSession* session, float* out, uint32_t max_bones);
/* Sets up skater -> Snake retargeting: Snake's rest skeleton from the MGS3 model snake_mdl_path
 * (assets/mdl/us/sna_def.mdl), the skater's bind pose from the session's private/skater.glb.
 * mirror_x nonzero reflects the skater left-right before driving Snake. 0 on success, -1. */
int mgsk_retarget_init(MgskSession* session, const char* snake_mdl_path, int mirror_x);
/* Writes up to max_bones of Snake's bone world matrices for the current skater pose (16 floats
 * each, MGS3 layout: rows X axis, Y axis, Z axis, translation (x, y, z, 1), millimetres). Needs
 * mgsk_retarget_init. Returns the count written (57 for sna_def.mdl), or -1. */
int mgsk_snake_pose(const MgskSession* session, float* out, uint32_t max_bones);
/* Writes the engine's chase camera: position (3, metres, +Y up), basis (9, three columns),
 * vertical field of view in degrees (1) = 13 floats. 0 on success, -1 if no camera frame yet. */
int mgsk_camera(const MgskSession* session, float* out);
/* Writes up to max_bodies board part world transforms (16 floats each, column-major) to out,
 * in MGSK_BODY_* order. Returns the count written, or -1. */
int mgsk_board_pose(const MgskSession* session, float* out, uint32_t max_bodies);
/* Reads board part `body` (MGSK_BODY_*) from assets_dir/private/skater.glb in that body's
 * local frame; draw it with the body's mgsk_board_pose transform. The deck part has the deck
 * and baseplates, each truck its hanger, each wheel itself. Triangles are index triples,
 * counter-clockwise from outside, engine axes (+Y up, +Z to the nose, right-handed).
 * Sets *out_vertices/*out_indices to the part's sizes and copies it only if both buffers fit.
 * Returns 1 if copied, 0 if only sizes were reported, -1 on failure. Parses the file on
 * every call: load once. */
int mgsk_board_mesh(const char* assets_dir, uint32_t body, MgskBoardVertex* vertices, uint32_t max_vertices,
                    uint16_t* indices, uint32_t max_indices, uint32_t* out_vertices, uint32_t* out_indices);

#ifdef __cplusplus
}
#endif
