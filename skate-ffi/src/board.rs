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

/// A run of a part's indices drawn with one texture; layout shared with `MgskBoardRange`.
#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct Range {
    pub texture: u32,
    pub first_index: u32,
    pub index_count: u32,
}

#[derive(Default)]
pub struct Part {
    pub vertices: Vec<Vertex>,
    /// Grouped by texture, in `ranges` order.
    pub indices: Vec<u16>,
    pub ranges: Vec<Range>,
}

/// A board texture, RGBA8 rows top to bottom (glTF's UV origin is the image's top-left).
pub struct Texture {
    pub width: u32,
    pub height: u32,
    pub rgba: Vec<u8>,
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
    // Per part, runs of (glTF material, triangle indices); merged into `indices` and `ranges` below.
    let mut by_material: [Vec<(usize, Vec<u16>)>; 7] = Default::default();
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
        let material = material_of(primitive);
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
                let runs = &mut by_material[b];
                if runs.last().is_none_or(|(m, _)| *m != material) {
                    runs.push((material, Vec::new()));
                }
                runs.last_mut().unwrap().1.push(remap[v][b]);
            }
        }
    }
    // Texture slots: the board's materials in glTF order (as `textures` returns them).
    let mut materials: Vec<usize> = by_material.iter().flatten().map(|(m, _)| *m).collect();
    materials.sort_unstable();
    materials.dedup();
    for (part, runs) in parts.iter_mut().zip(by_material) {
        for (texture, &material) in materials.iter().enumerate() {
            let first = part.indices.len() as u32;
            for (_, idx) in runs.iter().filter(|(m, _)| *m == material) {
                part.indices.extend_from_slice(idx);
            }
            let index_count = part.indices.len() as u32 - first;
            if index_count > 0 {
                part.ranges.push(Range { texture: texture as u32, first_index: first, index_count });
            }
        }
    }
    if parts[DECK].indices.is_empty() {
        return Err(format!("{} has no skateboard triangles", path.display()));
    }
    Ok(parts)
}

fn material_of(primitive: &Value) -> usize {
    primitive["material"].as_u64().map_or(usize::MAX, |m| m as usize)
}

/// The base-colour texture of each board material, in texture-slot order (`Range::texture`).
/// A material without one gets a 1x1 texel of its base colour factor.
pub fn textures(assets: &Path) -> Result<Vec<Texture>, String> {
    let path = assets.join("private").join("skater.glb");
    let parts = load(assets)?;
    let slots = parts.iter().flat_map(|p| p.ranges.iter()).map(|r| r.texture as usize + 1).max().unwrap_or(0);
    let bytes = std::fs::read(&path).map_err(|e| format!("{}: {e}", path.display()))?;
    let glb = Glb::parse(&bytes).map_err(|e| format!("{}: {e}", path.display()))?;
    // The same material list `load` assigns slots from.
    let used = board_materials(&glb)?;
    if used.len() != slots {
        return Err(format!("board materials ({}) and texture slots ({slots}) disagree", used.len()));
    }
    used.into_iter()
        .map(|m| {
            let pbr = &glb.json["materials"][m]["pbrMetallicRoughness"];
            let factor: Vec<f64> = pbr["baseColorFactor"]
                .as_array()
                .map_or(vec![1.; 4], |a| a.iter().filter_map(Value::as_f64).collect());
            let Some(tex) = pbr["baseColorTexture"]["index"].as_u64() else {
                let texel = (0..4).map(|i| (factor.get(i).copied().unwrap_or(1.).clamp(0., 1.) * 255.).round() as u8).collect();
                return Ok(Texture { width: 1, height: 1, rgba: texel });
            };
            let image = &glb.json["images"][index(&glb.json["textures"][tex as usize]["source"])?];
            let data = glb.view(index(&image["bufferView"])?)?;
            let (width, height, mut rgba) =
                crate::png::decode(data).map_err(|e| format!("{}: board texture: {e}", path.display()))?;
            if factor.iter().take(4).any(|&f| f != 1.) {
                for px in rgba.chunks_exact_mut(4) {
                    for (c, f) in px.iter_mut().zip(&factor) {
                        *c = (f64::from(*c) * f).round().clamp(0., 255.) as u8;
                    }
                }
            }
            Ok(Texture { width, height, rgba })
        })
        .collect()
}

/// Materials of the primitives that hold board triangles (a vertex whose dominant joint is a
/// board joint), sorted: the texture slots `load` assigns.
fn board_materials(glb: &Glb) -> Result<Vec<usize>, String> {
    let joints = glb.json["skins"][0]["joints"].as_array().ok_or("skin has no joints")?;
    let board: Vec<bool> = joints
        .iter()
        .map(|j| {
            index(j).is_ok_and(|n| glb.json["nodes"][n]["name"].as_str().is_some_and(|name| JOINTS.contains(&name)))
        })
        .collect();
    let mut used = Vec::new();
    for primitive in glb.json["meshes"][0]["primitives"].as_array().ok_or("skater.glb has no mesh")? {
        let attributes = &primitive["attributes"];
        let js = glb.components(index(&attributes["JOINTS_0"])?)?;
        let ws = glb.components(index(&attributes["WEIGHTS_0"])?)?;
        let indices = glb.components(index(&primitive["indices"])?)?;
        let on_board = |v: usize| {
            let (j, w) = (&js[v * 4..v * 4 + 4], &ws[v * 4..v * 4 + 4]);
            let k = (0..4).fold(0, |best, k| if w[k] > w[best] { k } else { best });
            board.get(j[k] as usize).copied().unwrap_or(false)
        };
        // `load` keeps a triangle when any corner is on the board.
        if indices.chunks_exact(3).any(|t| t.iter().any(|&v| on_board(v as usize))) {
            used.push(material_of(primitive));
        }
    }
    used.sort_unstable();
    used.dedup();
    Ok(used)
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

    /// The bytes of a buffer view (embedded images).
    pub(crate) fn view(&self, view: usize) -> Result<&'a [u8], String> {
        let v = &self.json["bufferViews"][view];
        let start = v["byteOffset"].as_u64().unwrap_or(0) as usize;
        let len = index(&v["byteLength"])?;
        self.blob.get(start..start + len).ok_or_else(|| "buffer view outside the GLB".to_string())
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
