//! C API over the Skate 3 Rust engine, loaded by `MetalGearSkate.asi` inside MGS3.
//! Mirrored in `include/skate_ffi.h`; keep the two in sync.
//!
//! Every function catches panics and reports failure through its return value plus
//! `mgsk_last_error`, because unwinding into the game's C++ is undefined behaviour.

use std::cell::RefCell;
use std::ffi::{CStr, CString, c_char};
use std::panic::{AssertUnwindSafe, catch_unwind};
use std::path::PathBuf;
use std::sync::Mutex;

use skate_host::bridge::{Controls, Session};

mod board;
mod geom;
mod rails;
mod retarget;

static VERSION: &CStr = c"skate-ffi 0.1.0";

thread_local! {
    static LAST_ERROR: RefCell<CString> = RefCell::new(CString::default());
}

/// Per-session state the engine doesn't hold, keyed by session address: the assets folder it
/// was created from and the retargeting set up by `mgsk_retarget_init`.
struct Extra {
    assets: PathBuf,
    retarget: Option<retarget::Retarget>,
}

static EXTRAS: Mutex<std::collections::BTreeMap<usize, Extra>> = Mutex::new(std::collections::BTreeMap::new());

/// Grind rails installed by the last `mgsk_install_geom`, keyed by session address.
static RAIL_COUNTS: Mutex<std::collections::BTreeMap<usize, i32>> = Mutex::new(std::collections::BTreeMap::new());

fn rail_counts() -> std::sync::MutexGuard<'static, std::collections::BTreeMap<usize, i32>> {
    RAIL_COUNTS.lock().unwrap_or_else(|poisoned| poisoned.into_inner())
}

fn extras() -> std::sync::MutexGuard<'static, std::collections::BTreeMap<usize, Extra>> {
    EXTRAS.lock().unwrap_or_else(|poisoned| poisoned.into_inner())
}

fn set_error(message: impl Into<String>) {
    let message = CString::new(message.into().replace('\0', " ")).unwrap_or_default();
    LAST_ERROR.with(|e| *e.borrow_mut() = message);
}

/// Runs `f`, turning `Err` and panics into `fail` plus a stored error message.
fn guard<T>(fail: T, f: impl FnOnce() -> Result<T, String>) -> T {
    match catch_unwind(AssertUnwindSafe(f)) {
        Ok(Ok(value)) => value,
        Ok(Err(message)) => {
            set_error(message);
            fail
        }
        Err(panic) => {
            let message = panic
                .downcast_ref::<&str>()
                .map(|s| s.to_string())
                .or_else(|| panic.downcast_ref::<String>().cloned())
                .unwrap_or_else(|| "panic".into());
            set_error(format!("panic: {message}"));
            fail
        }
    }
}

/// Controller state in XInput's layout.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct MgskControls {
    pub buttons: u16,
    pub left_trigger: u8,
    pub right_trigger: u8,
    pub left: [i16; 2],
    pub right: [i16; 2],
}

/// Static string identifying this build. Never null.
#[unsafe(no_mangle)]
pub extern "C" fn mgsk_version() -> *const c_char {
    VERSION.as_ptr()
}

/// Message for the last failure on this thread. Never null; empty if none.
#[unsafe(no_mangle)]
pub extern "C" fn mgsk_last_error() -> *const c_char {
    LAST_ERROR.with(|e| e.borrow().as_ptr())
}

/// Loads the converted Skate 3 data from `assets_dir` and builds a world from
/// `triangle_count` triangles (9 floats each, in the engine's space). Null on failure.
///
/// # Safety
/// `assets_dir` is a NUL-terminated UTF-8 path; `triangles` points at `triangle_count * 9` floats.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mgsk_session_new(
    assets_dir: *const c_char,
    triangles: *const f32,
    triangle_count: u32,
    spawn: *const f32,
    heading: f32,
) -> *mut Session {
    guard(std::ptr::null_mut(), || {
        if assets_dir.is_null() || spawn.is_null() {
            return Err("null argument".into());
        }
        let root = path_from(assets_dir)?;
        let tris = triangles_from(triangles, triangle_count)?;
        let spawn = unsafe { [*spawn, *spawn.add(1), *spawn.add(2)] };
        let session = Box::into_raw(Box::new(Session::new(&root, tris, Vec::new(), spawn, heading)?));
        extras().insert(session as usize, Extra { assets: root, retarget: None });
        Ok(session)
    })
}

/// # Safety
/// `session` came from `mgsk_session_new` and is not used afterwards. Null is ignored.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mgsk_session_free(session: *mut Session) {
    if !session.is_null() {
        guard((), || {
            extras().remove(&(session as usize));
            rail_counts().remove(&(session as usize));
            drop(unsafe { Box::from_raw(session) });
            Ok(())
        });
    }
}

fn triangles_from(triangles: *const f32, count: u32) -> Result<Vec<[[f32; 3]; 3]>, String> {
    if count == 0 {
        return Ok(Vec::new());
    }
    if triangles.is_null() {
        return Err("null triangles".into());
    }
    let flat = unsafe { std::slice::from_raw_parts(triangles, count as usize * 9) };
    Ok(flat
        .chunks_exact(9)
        .map(|t| [[t[0], t[1], t[2]], [t[3], t[4], t[5]], [t[6], t[7], t[8]]])
        .collect())
}

/// Replaces the world the skater rides with `triangle_count` triangles (9 floats each).
/// Builds on the calling thread; takes effect from the next tick. 0 on success.
///
/// # Safety
/// `session` is a live session not used concurrently; `triangles` points at `triangle_count * 9` floats.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mgsk_install_world(session: *mut Session, triangles: *const f32, triangle_count: u32) -> i32 {
    guard(-1, || {
        let session = unsafe { session.as_mut() }.ok_or("null session")?;
        let prepared = session
            .collision_builder()
            .build(triangles_from(triangles, triangle_count)?, Vec::new())?;
        session.install_collision(prepared)?;
        Ok(0)
    })
}

fn path_from(path: *const c_char) -> Result<PathBuf, String> {
    if path.is_null() {
        return Err("null path".into());
    }
    Ok(PathBuf::from(unsafe { CStr::from_ptr(path) }.to_str().map_err(|e| e.to_string())?))
}

/// The solid triangles of an MGS3 `.geom` file, in millimetres on the game's axes.
fn read_geom(path: *const c_char) -> Result<Vec<geom::Triangle>, String> {
    let path = path_from(path)?;
    let data = std::fs::read(&path).map_err(|e| format!("{}: {e}", path.display()))?;
    Ok(geom::decode(&data).map_err(|e| format!("{}: {e}", path.display()))?.triangles)
}

fn count_i32(n: usize) -> Result<i32, String> {
    i32::try_from(n).map_err(|_| format!("{n} triangles do not fit an int"))
}

/// Replaces the world the skater rides with the solid collision of an MGS3 `.geom` file,
/// every coordinate (mm, game axes) multiplied by `units_per_mm`, plus grind rails found on its
/// ledges, kerbs and railings (see `rails.rs`; `mgsk_rail_count` gives how many). Builds on the
/// calling thread; takes effect from the next tick. Returns the triangle count, or -1 on failure.
///
/// # Safety
/// `session` is a live session not used concurrently; `geom_path` is a NUL-terminated UTF-8 path.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mgsk_install_geom(session: *mut Session, geom_path: *const c_char, units_per_mm: f32) -> i32 {
    guard(-1, || {
        let session = unsafe { session.as_mut() }.ok_or("null session")?;
        if !(units_per_mm.is_finite() && units_per_mm > 0.) {
            return Err(format!("bad units_per_mm {units_per_mm}"));
        }
        let mut tris = read_geom(geom_path)?;
        for c in tris.iter_mut().flatten().flatten() {
            *c *= units_per_mm;
        }
        let count = count_i32(tris.len())?;
        let rails = geom_rails(&tris, units_per_mm);
        let rail_count = count_i32(rails.len())?;
        let prepared = session.collision_builder().build(tris, rails)?;
        session.install_collision(prepared)?;
        rail_counts().insert(std::ptr::from_mut(session) as usize, rail_count);
        Ok(count)
    })
}

/// Grind rails over `tris` (game axes, `units_per_mm` engine units per mm). The rail finder
/// works in metres, so it sees the stage at real scale whatever the engine's units are.
fn geom_rails(tris: &[geom::Triangle], units_per_mm: f32) -> Vec<Vec<[f32; 3]>> {
    let to_m = 1. / (1000. * units_per_mm);
    let metres: Vec<geom::Triangle> = tris.iter().map(|t| t.map(|p| p.map(|c| c * to_m))).collect();
    let (rails, _) = rails::find(&metres);
    rails.into_iter().map(|r| r.into_iter().map(|p| p.map(|c| c / to_m)).collect()).collect()
}

/// Number of grind rails the last `mgsk_install_geom` gave `session` (0 before any), or -1.
///
/// # Safety
/// `session` is a live session.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mgsk_rail_count(session: *const Session) -> i32 {
    guard(-1, || {
        let session = unsafe { session.as_ref() }.ok_or("null session")?;
        Ok(rail_counts().get(&(std::ptr::from_ref(session) as usize)).copied().unwrap_or(0))
    })
}

/// The grind rails `mgsk_install_geom` would find in an MGS3 `.geom` file, as segments: writes
/// up to `capacity` segments (6 floats each: start, end; coordinates as `mgsk_install_geom`
/// with `units_per_mm`) to `out` and returns the total segment count, or -1. If `rail_count`
/// is not null it receives the number of rails. For tests and debugging.
///
/// # Safety
/// `geom_path` is a NUL-terminated UTF-8 path; `out` has room for `capacity * 6` floats (may be
/// null when `capacity` is 0); `rail_count` is null or valid.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mgsk_geom_rails(
    geom_path: *const c_char,
    units_per_mm: f32,
    out: *mut f32,
    capacity: u32,
    rail_count: *mut i32,
) -> i32 {
    guard(-1, || {
        if !(units_per_mm.is_finite() && units_per_mm > 0.) {
            return Err(format!("bad units_per_mm {units_per_mm}"));
        }
        let mut tris = read_geom(geom_path)?;
        for c in tris.iter_mut().flatten().flatten() {
            *c *= units_per_mm;
        }
        let rails = geom_rails(&tris, units_per_mm);
        let segments: Vec<[f32; 6]> = rails
            .iter()
            .flat_map(|r| r.windows(2).map(|w| [w[0][0], w[0][1], w[0][2], w[1][0], w[1][1], w[1][2]]))
            .collect();
        let n = segments.len().min(capacity as usize);
        if n > 0 {
            if out.is_null() {
                return Err("null out".into());
            }
            unsafe { std::ptr::copy_nonoverlapping(segments.as_flattened().as_ptr(), out, n * 6) };
        }
        if !rail_count.is_null() {
            unsafe { *rail_count = count_i32(rails.len())? };
        }
        count_i32(segments.len())
    })
}

/// Number of solid triangles in an MGS3 `.geom` file, or -1 on failure. Decodes only.
///
/// # Safety
/// `geom_path` is a NUL-terminated UTF-8 path.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mgsk_geom_triangle_count(geom_path: *const c_char) -> i32 {
    guard(-1, || count_i32(read_geom(geom_path)?.len()))
}

/// Writes up to `capacity` solid triangles of an MGS3 `.geom` file (9 floats each, mm, game
/// axes) to `out` and returns the total count, or -1 on failure. For tests and debugging.
///
/// # Safety
/// `geom_path` is a NUL-terminated UTF-8 path; `out` has room for `capacity * 9` floats
/// (may be null when `capacity` is 0).
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mgsk_geom_triangles(geom_path: *const c_char, out: *mut f32, capacity: u32) -> i32 {
    guard(-1, || {
        let tris = read_geom(geom_path)?;
        let n = tris.len().min(capacity as usize);
        if n > 0 {
            if out.is_null() {
                return Err("null out".into());
            }
            let flat = tris[..n].as_flattened().as_flattened();
            unsafe { std::ptr::copy_nonoverlapping(flat.as_ptr(), out, flat.len()) };
        }
        count_i32(tris.len())
    })
}

/// Seconds per engine tick, or -1 on failure.
///
/// # Safety
/// `session` is a live session.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mgsk_period(session: *const Session) -> f32 {
    guard(-1.0, || Ok(unsafe { session.as_ref() }.ok_or("null session")?.period()))
}

/// Puts the skater on the board at `position` (3 floats) facing `heading` radians. 0 on success.
///
/// # Safety
/// `session` is a live session; `position` points at 3 floats.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mgsk_activate(session: *mut Session, position: *const f32, heading: f32) -> i32 {
    guard(-1, || {
        let session = unsafe { session.as_mut() }.ok_or("null session")?;
        if position.is_null() {
            return Err("null position".into());
        }
        let p = unsafe { [*position, *position.add(1), *position.add(2)] };
        session.activate(p, heading)?;
        Ok(0)
    })
}

/// Advances one engine tick with this controller state. 0 on success.
///
/// # Safety
/// `session` is a live session; `controls` points at one `MgskControls`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mgsk_tick(session: *mut Session, controls: *const MgskControls) -> i32 {
    guard(-1, || {
        let session = unsafe { session.as_mut() }.ok_or("null session")?;
        let c = unsafe { controls.as_ref() }.ok_or("null controls")?;
        session.tick(Controls {
            buttons: c.buttons,
            triggers: [c.left_trigger, c.right_trigger],
            left: c.left,
            right: c.right,
        })?;
        Ok(0)
    })
}

/// Writes the skater root transform (16 floats, column-major) to `out`. 0 on success.
///
/// # Safety
/// `session` is a live session; `out` has room for 16 floats.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mgsk_root(session: *const Session, out: *mut f32) -> i32 {
    guard(-1, || {
        let session = unsafe { session.as_ref() }.ok_or("null session")?;
        if out.is_null() {
            return Err("null out".into());
        }
        let m = session.pose().root.to_cols_array();
        unsafe { std::ptr::copy_nonoverlapping(m.as_ptr(), out, 16) };
        Ok(0)
    })
}

/// Writes the engine's chase camera to `out`: position (3, engine units = metres, +Y up), basis
/// (9, three columns), vertical field of view in degrees (1): 13 floats. Returns 0, or -1 when
/// the camera has no frame yet (or on error).
///
/// # Safety
/// `session` is a live session; `out` has room for 13 floats.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mgsk_camera(session: *const Session, out: *mut f32) -> i32 {
    guard(-1, || {
        let session = unsafe { session.as_ref() }.ok_or("null session")?;
        if out.is_null() {
            return Err("null out".into());
        }
        let (position, basis, fov) = session.pose().camera.ok_or("no camera frame yet")?;
        let mut v = [0f32; 13];
        v[..3].copy_from_slice(&position.to_array());
        v[3..12].copy_from_slice(&basis.to_cols_array());
        v[12] = fov;
        unsafe { std::ptr::copy_nonoverlapping(v.as_ptr(), out, 13) };
        Ok(0)
    })
}

/// Writes the world transforms of up to `max_bodies` board parts (16 floats each,
/// column-major) to `out`, in engine body order: right front, left front, right back and
/// left back wheel, front truck, back truck, deck. Returns the count written, or -1.
///
/// # Safety
/// `session` is a live session; `out` has room for `max_bodies * 16` floats (may be null
/// when `max_bodies` is 0).
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mgsk_board_pose(session: *const Session, out: *mut f32, max_bodies: u32) -> i32 {
    guard(-1, || {
        let session = unsafe { session.as_ref() }.ok_or("null session")?;
        let parts = session.board_parts();
        let n = parts.len().min(max_bodies as usize);
        if n > 0 {
            if out.is_null() {
                return Err("null out".into());
            }
            for (i, part) in parts[..n].iter().enumerate() {
                unsafe { std::ptr::copy_nonoverlapping(part.to_cols_array().as_ptr(), out.add(i * 16), 16) };
            }
        }
        count_i32(n)
    })
}

/// Reads the skateboard part `body` (engine body order, as in `mgsk_board_pose`; 6 is the
/// deck) from `assets_dir/private/skater.glb`, in that body's local frame, so drawing it with
/// the body's transform places it. The deck part carries the deck and baseplates; each
/// truck its hanger; each wheel itself. Triangles are `indices` triples, counter-clockwise
/// seen from outside, on the engine's right-handed axes (+Y up, +Z to the nose).
///
/// Sets `*out_vertices` and `*out_indices` to the part's sizes, then copies the part only if
/// both buffers are large enough. Returns 1 if copied, 0 if only the sizes were reported,
/// -1 on failure. Parses the GLB on every call: load once, not per frame.
///
/// # Safety
/// `assets_dir` is a NUL-terminated UTF-8 path; `vertices` has room for `max_vertices` and
/// `indices` for `max_indices` (either may be null when its capacity is 0); `out_vertices`
/// and `out_indices` are valid.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mgsk_board_mesh(
    assets_dir: *const c_char,
    body: u32,
    vertices: *mut board::Vertex,
    max_vertices: u32,
    indices: *mut u16,
    max_indices: u32,
    out_vertices: *mut u32,
    out_indices: *mut u32,
) -> i32 {
    guard(-1, || {
        if out_vertices.is_null() || out_indices.is_null() {
            return Err("null out".into());
        }
        let parts = board::load(&path_from(assets_dir)?)?;
        let part = parts.get(body as usize).ok_or_else(|| format!("no board body {body}"))?;
        let (nv, ni) = (part.vertices.len(), part.indices.len());
        unsafe {
            *out_vertices = nv as u32;
            *out_indices = ni as u32;
        }
        if nv > max_vertices as usize || ni > max_indices as usize {
            return Ok(0);
        }
        if vertices.is_null() || indices.is_null() {
            return Err("null vertices or indices".into());
        }
        unsafe {
            std::ptr::copy_nonoverlapping(part.vertices.as_ptr(), vertices, nv);
            std::ptr::copy_nonoverlapping(part.indices.as_ptr(), indices, ni);
        }
        Ok(1)
    })
}

/// Number of skater bones in `mgsk_bones` / `mgsk_bone_name`, or -1 on failure.
///
/// # Safety
/// `session` is a live session.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mgsk_bone_count(session: *const Session) -> i32 {
    guard(-1, || {
        let session = unsafe { session.as_ref() }.ok_or("null session")?;
        count_i32(session.pose().bones.len())
    })
}

/// Copies the name of skater bone `index` (NUL-terminated, truncated to fit) into `out`, which
/// holds `capacity` bytes. Returns the name's full length in bytes without the NUL, or -1.
///
/// # Safety
/// `session` is a live session; `out` has room for `capacity` bytes (may be null when
/// `capacity` is 0).
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mgsk_bone_name(session: *const Session, index: u32, out: *mut c_char, capacity: u32) -> i32 {
    guard(-1, || {
        let session = unsafe { session.as_ref() }.ok_or("null session")?;
        let names = session.pose().names;
        let name = names.get(index as usize).ok_or_else(|| format!("no bone {index}"))?;
        if capacity > 0 {
            if out.is_null() {
                return Err("null out".into());
            }
            let n = name.len().min(capacity as usize - 1);
            unsafe {
                std::ptr::copy_nonoverlapping(name.as_ptr().cast::<c_char>(), out, n);
                *out.add(n) = 0;
            }
        }
        count_i32(name.len())
    })
}

/// Writes up to `max_bones` skater bone transforms (16 floats each, column-major) to `out`,
/// in `mgsk_bone_name` order. They are relative to the skater root: the world transform of
/// bone `i` is `root * bone[i]`, with `root` from `mgsk_root`. Returns the count written, or -1.
///
/// # Safety
/// `session` is a live session; `out` has room for `max_bones * 16` floats (may be null when
/// `max_bones` is 0).
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mgsk_bones(session: *const Session, out: *mut f32, max_bones: u32) -> i32 {
    guard(-1, || {
        let session = unsafe { session.as_ref() }.ok_or("null session")?;
        let bones = session.pose().bones;
        let n = bones.len().min(max_bones as usize);
        if n > 0 {
            if out.is_null() {
                return Err("null out".into());
            }
            for (i, bone) in bones[..n].iter().enumerate() {
                unsafe { std::ptr::copy_nonoverlapping(bone.to_cols_array().as_ptr(), out.add(i * 16), 16) };
            }
        }
        count_i32(n)
    })
}

/// Sets up skater -> Snake retargeting for `session`: reads Snake's rest skeleton from the MGS3
/// model `snake_mdl_path` (`assets/mdl/us/sna_def.mdl`) and the skater's bind pose from the
/// session's `private/skater.glb`. With `mirror_x` nonzero, the skater's pose is reflected
/// left-right before it drives Snake. 0 on success.
///
/// # Safety
/// `session` is a live session not used concurrently; `snake_mdl_path` is a NUL-terminated
/// UTF-8 path.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mgsk_retarget_init(session: *mut Session, snake_mdl_path: *const c_char, mirror_x: i32) -> i32 {
    guard(-1, || {
        let session = unsafe { session.as_ref() }.ok_or("null session")?;
        let key = std::ptr::from_ref(session) as usize;
        let assets = extras().get(&key).ok_or("unknown session")?.assets.clone();
        let path = path_from(snake_mdl_path)?;
        let data = std::fs::read(&path).map_err(|e| format!("{}: {e}", path.display()))?;
        let snake = retarget::read_mdl_bones(&data).map_err(|e| format!("{}: {e}", path.display()))?;
        let bind = retarget::read_skater_bind(&assets.join("private").join("skater.glb"))?;
        let retarget = retarget::Retarget::new(snake, &session.pose().names, &bind, mirror_x != 0)?;
        extras().get_mut(&key).ok_or("unknown session")?.retarget = Some(retarget);
        Ok(0)
    })
}

/// Writes up to `max_bones` of Snake's bone world matrices for the current skater pose to
/// `out`, 16 floats each in MGS3's layout: rows X axis, Y axis, Z axis, translation
/// `(x, y, z, 1)`, millimetres, engine/game axes. Needs `mgsk_retarget_init` first. Returns the
/// count written (57 for `sna_def.mdl` when `max_bones` allows), or -1.
///
/// # Safety
/// `session` is a live session; `out` has room for `max_bones * 16` floats (may be null when
/// `max_bones` is 0).
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mgsk_snake_pose(session: *const Session, out: *mut f32, max_bones: u32) -> i32 {
    guard(-1, || {
        let session = unsafe { session.as_ref() }.ok_or("null session")?;
        let extras = extras();
        let retarget = extras
            .get(&(std::ptr::from_ref(session) as usize))
            .and_then(|e| e.retarget.as_ref())
            .ok_or("mgsk_retarget_init has not succeeded for this session")?;
        let pose = session.pose();
        let bones: Vec<[f32; 16]> = pose.bones.iter().map(|b| b.to_cols_array()).collect();
        let snake = retarget.pose(&pose.root.to_cols_array(), &bones)?;
        let n = snake.len().min(max_bones as usize);
        if n > 0 {
            if out.is_null() {
                return Err("null out".into());
            }
            unsafe { std::ptr::copy_nonoverlapping(snake[..n].as_flattened().as_ptr(), out, n * 16) };
        }
        count_i32(n)
    })
}

/// Copies the skater's current state name (the engine's player state, e.g. `Riding`, or a
/// grind state while grinding; NUL-terminated, truncated to fit) into `out`, which holds
/// `capacity` bytes. Returns the name's full length in bytes without the NUL, or -1.
///
/// # Safety
/// `session` is a live session; `out` has room for `capacity` bytes (may be null when
/// `capacity` is 0).
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mgsk_state(session: *const Session, out: *mut c_char, capacity: u32) -> i32 {
    guard(-1, || {
        let session = unsafe { session.as_ref() }.ok_or("null session")?;
        let state = session.pose().state;
        if capacity > 0 {
            if out.is_null() {
                return Err("null out".into());
            }
            let n = state.len().min(capacity as usize - 1);
            unsafe {
                std::ptr::copy_nonoverlapping(state.as_ptr().cast::<c_char>(), out, n);
                *out.add(n) = 0;
            }
        }
        count_i32(state.len())
    })
}
