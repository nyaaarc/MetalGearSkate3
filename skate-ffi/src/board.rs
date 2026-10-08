//! The skateboard mesh from the converted skater GLB (`private/skater.glb`), split by the
//! engine's seven board bodies and expressed in each body's local frame, so a part is
//! drawn with its transform from `Session::board_parts`.
//!
//! The GLB holds skater and board as one skinned mesh. Board vertices are those skinned to
//! the `SKATEBOARD_ROOT` subtree (rigidly: every weight is 1). The deck frame is the
//! `SKATEBOARD_ROOT` bind with the GLB's Blender bone basis removed; it matches the engine's
//! deck body (origin 0.089 m above the ground, +Y up, +Z to the nose, right wheels on -X).
//! The other parts use the engine's stock rest poses relative to the deck.
//! The GLB reading follows the mashup's `crates/assets/src/skate_board.rs` (Apache-2.0).
use std::path::Path;

use serde_json::Value;
use skate_core::physics::drive_frames::{
    AuthoredTransformInputs, RetailAffineTransform, authored_body_transforms, default_truck_transforms,
};

/// Vertex layout shared with `MgskBoardVertex` in `skate_ffi.h`.
#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct Vertex {
    pub position: [f32; 3],
    pub normal: [f32; 3],
    pub uv: [f32; 2],
}

#[derive(Default)]
pub struct Part {
    pub vertices: Vec<Vertex>,
    pub indices: Vec<u16>,
}

/// GLB joint name of each engine body, in `BodyId` order.
const JOINTS: [&str; 7] = [
    "RIGHT_WHEELFRONT",
    "LEFT_WHEELFRONT",
    "RIGHT_WHEELBACK",
    "LEFT_WHEELBACK",
    "TRUCK_FRONT",
    "TRUCK_BACK",
    "SKATEBOARD_ROOT",
];
const DECK: usize = 6;

/// Column-major 4x4, as glTF stores it.
type Mat = [f64; 16];

fn mul(a: &Mat, b: &Mat) -> Mat {
    std::array::from_fn(|i| {
        let (col, row) = (i / 4, i % 4);
        (0..4).map(|k| a[k * 4 + row] * b[col * 4 + k]).sum()
    })
}

fn apply(m: &Mat, v: [f32; 3], w: f64) -> [f64; 3] {
    let v = v.map(f64::from);
    std::array::from_fn(|r| m[r] * v[0] + m[4 + r] * v[1] + m[8 + r] * v[2] + m[12 + r] * w)
}

/// Body pose relative to the deck when the board is at rest: the stock authored positions,
/// with each truck's tilted axis frame (wheels share their truck's; their spin is free).
fn rest_frames() -> [RetailAffineTransform; 7] {
    let mut rest = authored_body_transforms(AuthoredTransformInputs::STOCK);
    // calculate_truck_transforms returns the back truck first, then the front.
    let [back, front] = default_truck_transforms();
    for (i, truck) in [(0, front), (1, front), (2, back), (3, back), (4, front), (5, back)] {
        rest[i].basis = truck.basis;
    }
    rest[DECK] = RetailAffineTransform::IDENTITY;
    rest
}

/// Inverse of a rigid frame, as a matrix.
fn local_from_deck(frame: &RetailAffineTransform) -> Mat {
    let c = frame.basis.columns.map(|c| c.map(f64::from));
    let t = [frame.translation.x, frame.translation.y, frame.translation.z].map(f64::from);
    let dot = |a: [f64; 3]| a[0] * t[0] + a[1] * t[1] + a[2] * t[2];
    // Rows of the inverse are the basis columns.
    [
        c[0][0], c[1][0], c[2][0], 0.,
        c[0][1], c[1][1], c[2][1], 0.,
        c[0][2], c[1][2], c[2][2], 0.,
        -dot(c[0]), -dot(c[1]), -dot(c[2]), 1.,
    ]
}

/// The seven board parts, in `BodyId` order. A truck holds its hanger and axle; the deck
/// holds the deck plus the baseplates, which the GLB binds to the board root.
pub fn load(assets: &Path) -> Result<[Part; 7], String> {
    let path = assets.join("private").join("skater.glb");
    let bytes = std::fs::read(&path).map_err(|e| format!("{}: {e}", path.display()))?;
    let glb = Glb::parse(&bytes).map_err(|e| format!("{}: {e}", path.display()))?;

    let skin = &glb.json["skins"][0];
    let joints = skin["joints"].as_array().ok_or("skin has no joints")?;
    let joint_names: Vec<&str> = joints
        .iter()
        .map(|j| glb.json["nodes"][index(j)?]["name"].as_str().ok_or_else(|| "unnamed joint".to_string()))
        .collect::<Result<_, _>>()?;
    let joint_of = |name: &str| {
        joint_names.iter().position(|n| *n == name).ok_or_else(|| format!("skater.glb has no {name} joint"))
    };
    // GLB joint index -> body, for the board joints.
    let mut body_of_joint = vec![None; joint_names.len()];
    for (body, name) in JOINTS.iter().enumerate() {
        body_of_joint[joint_of(name)?] = Some(body);
    }

    let inverse_binds = glb.components(index(&skin["inverseBindMatrices"])?)?;
    let root = joint_of(JOINTS[DECK])?;
    let root_inverse_bind: Mat = inverse_binds.get(root * 16..root * 16 + 16).ok_or("short inverse binds")?.try_into().unwrap();
    // The Blender bone basis the converter put on every joint (local Y,Z = native -Z,Y).
    let blender_basis: Mat = [1., 0., 0., 0., 0., 0., -1., 0., 0., 1., 0., 0., 0., 0., 0., 1.];
    let deck_from_model = mul(&blender_basis, &root_inverse_bind);
    let rest = rest_frames();
    let local_from_model: [Mat; 7] = std::array::from_fn(|i| mul(&local_from_deck(&rest[i]), &deck_from_model));

    let mut parts: [Part; 7] = Default::default();
    let primitives = glb.json["meshes"][0]["primitives"].as_array().ok_or("skater.glb has no mesh")?;
    for primitive in primitives {
        let attributes = &primitive["attributes"];
        let joints = glb.components(index(&attributes["JOINTS_0"])?)?;
        let weights = glb.components(index(&attributes["WEIGHTS_0"])?)?;
        // Each vertex's dominant joint decides its part; board weights are rigid.
        let body: Vec<Option<usize>> = joints
            .chunks_exact(4)
            .zip(weights.chunks_exact(4))
            .map(|(j, w)| {
                let k = (0..4).fold(0, |best, k| if w[k] > w[best] { k } else { best });
                body_of_joint.get(j[k] as usize).copied().flatten()
            })
            .collect();
        if body.iter().all(Option::is_none) {
            continue;
        }
        let positions = glb.components(index(&attributes["POSITION"])?)?;
        let normals = glb.components(index(&attributes["NORMAL"])?)?;
        let uvs = glb.components(index(&attributes["TEXCOORD_0"])?)?;
        let indices = glb.components(index(&primitive["indices"])?)?;
        let count = body.len();
        if positions.len() != count * 3 || normals.len() != count * 3 || uvs.len() != count * 2 {
            return Err("skater.glb: vertex streams disagree".into());
        }
        // Per part, this primitive's vertex index -> part vertex index.
        let mut remap = vec![[u16::MAX; 7]; count];
        for tri in indices.chunks_exact(3) {
            let tri = [tri[0], tri[1], tri[2]].map(|i| i as usize);
            let bodies = tri.map(|v| body.get(v).copied().flatten());
            // A few triangles join a truck's hanger to its baseplate; they go with the part
            // most of their corners belong to, and stretch a little when the truck turns.
            let Some(b) = bodies.iter().flatten().copied().max_by_key(|b| bodies.iter().filter(|o| **o == Some(*b)).count())
            else {
                continue;
            };
            let part = &mut parts[b];
            for v in tri {
                if remap[v][b] == u16::MAX {
                    remap[v][b] = u16::try_from(part.vertices.len())
                        .ok()
                        .filter(|&n| n != u16::MAX)
                        .ok_or("board part has too many vertices for 16-bit indices")?;
                    let m = &local_from_model[b];
                    let at = |s: &[f64], n: usize| -> Vec<f32> { s[v * n..v * n + n].iter().map(|&x| x as f32).collect() };
                    let p = at(&positions, 3);
                    let n = at(&normals, 3);
                    let uv = at(&uvs, 2);
                    part.vertices.push(Vertex {
                        position: apply(m, [p[0], p[1], p[2]], 1.).map(|x| x as f32),
                        normal: apply(m, [n[0], n[1], n[2]], 0.).map(|x| x as f32),
                        uv: [uv[0], uv[1]],
                    });
                }
                part.indices.push(remap[v][b]);
            }
        }
    }
    if parts[DECK].indices.is_empty() {
        return Err(format!("{} has no skateboard triangles", path.display()));
    }
    Ok(parts)
}

pub(crate) fn index(value: &Value) -> Result<usize, String> {
    value.as_u64().map(|v| v as usize).ok_or_else(|| format!("expected an index, found {value}"))
}

pub(crate) struct Glb<'a> {
    pub(crate) json: Value,
    blob: &'a [u8],
}

impl<'a> Glb<'a> {
    pub(crate) fn parse(bytes: &'a [u8]) -> Result<Self, String> {
        let word = |at: usize| {
            bytes
                .get(at..at + 4)
                .map(|b| u32::from_le_bytes([b[0], b[1], b[2], b[3]]) as usize)
                .ok_or("truncated GLB")
        };
        if bytes.get(..4) != Some(b"glTF") {
            return Err("not a GLB file".into());
        }
        let json_len = word(12)?;
        let json = serde_json::from_slice(bytes.get(20..20 + json_len).ok_or("truncated GLB JSON")?)
            .map_err(|e| e.to_string())?;
        let blob_start = 28 + json_len;
        let blob = bytes.get(blob_start..blob_start + word(20 + json_len)?).ok_or("truncated GLB binary")?;
        Ok(Self { json, blob })
    }

    /// Every component of an accessor, widened to f64.
    pub(crate) fn components(&self, accessor: usize) -> Result<Vec<f64>, String> {
        let a = &self.json["accessors"][accessor];
        let count = index(&a["count"])?;
        let width = match a["type"].as_str() {
            Some("SCALAR") => 1,
            Some("VEC2") => 2,
            Some("VEC3") => 3,
            Some("VEC4") => 4,
            Some("MAT4") => 16,
            other => return Err(format!("unsupported accessor type {other:?}")),
        };
        let (size, read): (usize, fn(&[u8]) -> f64) = match a["componentType"].as_u64() {
            Some(5121) => (1, |b| f64::from(b[0])),
            Some(5123) => (2, |b| f64::from(u16::from_le_bytes([b[0], b[1]]))),
            Some(5125) => (4, |b| f64::from(u32::from_le_bytes([b[0], b[1], b[2], b[3]]))),
            Some(5126) => (4, |b| f64::from(f32::from_le_bytes([b[0], b[1], b[2], b[3]]))),
            other => return Err(format!("unsupported component type {other:?}")),
        };
        let view = &self.json["bufferViews"][index(&a["bufferView"])?];
        let start = view["byteOffset"].as_u64().unwrap_or(0) as usize + a["byteOffset"].as_u64().unwrap_or(0) as usize;
        let stride = view["byteStride"].as_u64().map_or(size * width, |s| s as usize);
        let mut out = Vec::with_capacity(count * width);
        for element in 0..count {
            for component in 0..width {
                let at = start + element * stride + component * size;
                out.push(read(self.blob.get(at..at + size).ok_or("accessor outside the GLB")?));
            }
        }
        Ok(out)
    }
}
