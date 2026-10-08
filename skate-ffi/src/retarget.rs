//! Skater -> Snake pose retargeting: turns the skater's bone transforms into world matrices
//! for Snake's 57-bone skeleton (`sna_def.mdl`), in the layout MGS3 keeps them.
//!
//! Snake's rest skeleton is translation only (T-pose, mm, +Y up, faces +Z), so his bind
//! rotation is identity for every bone. The skater's bind (A-pose) comes from
//! `private/skater.glb`. For a mapped Snake bone `i` driven by skater bone `s`:
//!
//! ```text
//! R_i = R_root * R_s(t) * R_s(bind)^-1 * C_i
//! ```
//!
//! where `C_i` is the shortest-arc rotation taking Snake's rest bone direction onto the
//! skater's bind bone direction (so T-pose vs A-pose cancels). Unmapped bones (palms,
//! fingers) keep their parent's rotation. Positions run down Snake's own hierarchy from his
//! rest offsets, so he keeps his proportions; the whole skeleton is then shifted so the
//! midpoint of his ankles lands on the midpoint of the skater's ankles (his legs are ~8%
//! longer, so anchoring at the hips would sink his feet into the board). See
//! `docs/formats.md` ("Snake's skeleton") and `docs/retarget.json`.
use std::collections::HashMap;
use std::path::Path;

use crate::board::{Glb, index};

pub type V3 = [f64; 3];
/// 3x3 rotation, column-major: `m[c]` is column `c` (the bone's local axis `c`).
pub type M3 = [V3; 3];

pub const IDENTITY: M3 = [[1., 0., 0.], [0., 1., 0.], [0., 0., 1.]];

pub fn add(a: V3, b: V3) -> V3 {
    [a[0] + b[0], a[1] + b[1], a[2] + b[2]]
}
pub fn sub(a: V3, b: V3) -> V3 {
    [a[0] - b[0], a[1] - b[1], a[2] - b[2]]
}
pub fn scale(a: V3, s: f64) -> V3 {
    [a[0] * s, a[1] * s, a[2] * s]
}
pub fn dot(a: V3, b: V3) -> f64 {
    a[0] * b[0] + a[1] * b[1] + a[2] * b[2]
}
pub fn cross(a: V3, b: V3) -> V3 {
    [a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]]
}
pub fn length(a: V3) -> f64 {
    dot(a, a).sqrt()
}
pub fn normalize(a: V3) -> V3 {
    let l = length(a);
    if l > 1e-12 { scale(a, 1. / l) } else { [0., 0., 0.] }
}

/// `m * v`.
pub fn apply(m: &M3, v: V3) -> V3 {
    add(add(scale(m[0], v[0]), scale(m[1], v[1])), scale(m[2], v[2]))
}
/// `a * b`.
pub fn mul(a: &M3, b: &M3) -> M3 {
    b.map(|col| apply(a, col))
}
pub fn transpose(m: &M3) -> M3 {
    std::array::from_fn(|c| std::array::from_fn(|r| m[r][c]))
}

/// The nearest rotation to `m` by Gram-Schmidt from the X axis (the skater's bone axis),
/// dropping the ~1% scale the engine's bone matrices carry.
pub fn orthonormalize(m: &M3) -> M3 {
    let x = normalize(m[0]);
    let y = normalize(sub(m[1], scale(x, dot(x, m[1]))));
    [x, y, cross(x, y)]
}

/// Rotation by `angle` radians about the unit `axis` (Rodrigues).
pub fn axis_angle(axis: V3, angle: f64) -> M3 {
    let (s, c) = angle.sin_cos();
    let [x, y, z] = axis;
    let t = 1. - c;
    [
        [t * x * x + c, t * x * y + s * z, t * x * z - s * y],
        [t * x * y - s * z, t * y * y + c, t * y * z + s * x],
        [t * x * z + s * y, t * y * z - s * x, t * z * z + c],
    ]
}

/// Shortest-arc rotation taking direction `from` onto direction `to`.
pub fn from_to(from: V3, to: V3) -> M3 {
    let (a, b) = (normalize(from), normalize(to));
    let axis = cross(a, b);
    let (s, c) = (length(axis), dot(a, b));
    if s < 1e-9 {
        if c > 0. {
            return IDENTITY;
        }
        // Opposite: half a turn about any axis perpendicular to `a`.
        let other = if a[0].abs() < 0.9 { [1., 0., 0.] } else { [0., 1., 0.] };
        return axis_angle(normalize(cross(a, other)), std::f64::consts::PI);
    }
    axis_angle(scale(axis, 1. / s), s.atan2(c))
}

/// Reflection through the YZ plane applied to a frame: `M * m * M`, M = diag(-1, 1, 1).
pub fn mirror_rotation(m: &M3) -> M3 {
    std::array::from_fn(|c| std::array::from_fn(|r| if (c == 0) != (r == 0) { -m[c][r] } else { m[c][r] }))
}
pub fn mirror_point(p: V3) -> V3 {
    [-p[0], p[1], p[2]]
}

/// Rotation and translation of a column-major 4x4.
fn split(m: &[f32; 16]) -> (M3, V3) {
    let f = |i: usize| f64::from(m[i]);
    ([[f(0), f(1), f(2)], [f(4), f(5), f(6)], [f(8), f(9), f(10)]], [f(12), f(13), f(14)])
}

/// One bone of Snake's rest skeleton: parent index (-1 for the root) and model-space rest
/// position in millimetres.
#[derive(Clone, Copy, Debug)]
pub struct SnakeBone {
    pub parent: i32,
    pub rest: V3,
}

/// The bone table of an MGS3 `.mdl` (layout in `tools/mdl_bones.py`).
pub fn read_mdl_bones(data: &[u8]) -> Result<Vec<SnakeBone>, String> {
    let u32_at = |at: usize| data.get(at..at + 4).map(|b| u32::from_le_bytes(b.try_into().unwrap())).ok_or("truncated mdl");
    let f32_at = |at: usize| u32_at(at).map(|w| f64::from(f32::from_bits(w)));
    if data.get(..4) != Some(b"MDC ") {
        return Err("not an MDC model".into());
    }
    let count = u32_at(4)? as usize;
    let offset = u32_at(0x18)? as usize | (u32_at(0x1C)? as usize) << 32;
    let mut bones = Vec::with_capacity(count);
    for i in 0..count {
        let at = offset + i * 0x50;
        let parent = u32_at(at + 8)? as i32;
        if !(-1..i as i32).contains(&parent) {
            return Err(format!("bone {i}: parent {parent} is not an earlier bone"));
        }
        bones.push(SnakeBone { parent, rest: [f32_at(at + 0x20)?, f32_at(at + 0x24)?, f32_at(at + 0x28)?] });
    }
    Ok(bones)
}

/// Bind rotation (engine bone basis) and position (metres) of each skater bone in
/// `skater.glb`, by joint name.
pub fn read_skater_bind(path: &Path) -> Result<HashMap<String, (M3, V3)>, String> {
    let bytes = std::fs::read(path).map_err(|e| format!("{}: {e}", path.display()))?;
    let glb = Glb::parse(&bytes).map_err(|e| format!("{}: {e}", path.display()))?;
    let skin = &glb.json["skins"][0];
    let joints = skin["joints"].as_array().ok_or("skin has no joints")?;
    let inverse_binds = glb.components(index(&skin["inverseBindMatrices"])?)?;
    let mut out = HashMap::new();
    for (j, joint) in joints.iter().enumerate() {
        let name = glb.json["nodes"][index(joint)?]["name"].as_str().ok_or("unnamed joint")?;
        let m: [f32; 16] = std::array::from_fn(|k| inverse_binds.get(j * 16 + k).copied().unwrap_or(0.) as f32);
        let (r, t) = split(&m);
        // Bind = inverse of the (rigid) inverse bind.
        let bind = transpose(&orthonormalize(&r));
        let position = scale(apply(&bind, t), -1.);
        // The converter's Blender bone basis (see board.rs): engine axes X, Y, Z are the GLB
        // joint's X, Z, -Y. Checked: parent-local child offsets then match mgsk_bones exactly.
        let engine = [bind[0], bind[2], scale(bind[1], -1.)];
        out.insert(name.to_string(), (engine, position));
    }
    Ok(out)
}

/// Skater bone driving each mapped Snake bone (`docs/retarget.json`). Snake's +X side is
/// the skater's LEFT side.
pub const MAP: [(usize, &str); 23] = [
    (0, "HIPS"),
    (1, "SPINE1"),
    (2, "SPINE3"),
    (3, "NECK"),
    (4, "HEAD"),
    (5, "LEFTSHOULDER"),
    (6, "LEFTARM"),
    (7, "LEFTFOREARM"),
    (8, "LEFTFOREARM"),
    (9, "LEFTHAND"),
    (10, "RIGHTSHOULDER"),
    (11, "RIGHTARM"),
    (12, "RIGHTFOREARM"),
    (13, "RIGHTFOREARM"),
    (14, "RIGHTHAND"),
    (15, "LEFTUPLEG"),
    (16, "LEFTLEG"),
    (17, "LEFTFOOT"),
    (18, "LEFTTOEBASE"),
    (19, "RIGHTUPLEG"),
    (20, "RIGHTLEG"),
    (21, "RIGHTFOOT"),
    (22, "RIGHTTOEBASE"),
];
/// Snake's ankle joints, the anchor that puts his feet where the skater's are.
const ANKLES: [usize; 2] = [17, 21];

fn swap_sides(name: &str) -> String {
    if let Some(rest) = name.strip_prefix("LEFT") {
        format!("RIGHT{rest}")
    } else if let Some(rest) = name.strip_prefix("RIGHT") {
        format!("LEFT{rest}")
    } else {
        name.to_string()
    }
}

pub struct Retarget {
    snake: Vec<SnakeBone>,
    /// Per Snake bone: skater pose index and `R_s(bind)^-1 * C_i`, if mapped.
    drive: Vec<Option<(usize, M3)>>,
    mirror: bool,
}

impl Retarget {
    /// `names` are the skater's bones in pose order; `bind` from `read_skater_bind`. With
    /// `mirror`, the skater's pose is reflected through its own YZ plane (and LEFT/RIGHT
    /// swapped) before driving Snake.
    pub fn new(snake: Vec<SnakeBone>, names: &[String], bind: &HashMap<String, (M3, V3)>, mirror: bool) -> Result<Self, String> {
        let mut source: Vec<Option<(usize, M3, V3)>> = vec![None; snake.len()];
        for (i, name) in MAP {
            if i >= snake.len() {
                return Err(format!("Snake skeleton has {} bones, expected at least 23", snake.len()));
            }
            let name = if mirror { swap_sides(name) } else { name.to_string() };
            let s = names.iter().position(|n| *n == name).ok_or_else(|| format!("skater has no {name} bone"))?;
            let &(r, p) = bind.get(&name).ok_or_else(|| format!("skater.glb has no {name} joint"))?;
            source[i] = Some(if mirror { (s, mirror_rotation(&r), mirror_point(p)) } else { (s, r, p) });
        }
        let first_mapped_child = |i: usize| (i + 1..snake.len()).find(|&j| snake[j].parent == i as i32 && source[j].is_some());
        let mut drive = vec![None; snake.len()];
        for i in 0..snake.len() {
            let Some((s, r, p)) = source[i] else { continue };
            // Bone direction: towards the first mapped child driven by a different skater
            // bone; for leaves (head, hands, toes), from the parent.
            let mut child = first_mapped_child(i);
            while let Some(c) = child.filter(|&c| source[c].unwrap().0 == s) {
                child = first_mapped_child(c);
            }
            let (snake_dir, skater_dir) = match child {
                Some(c) => (sub(snake[c].rest, snake[i].rest), sub(source[c].unwrap().2, p)),
                None => {
                    let parent = snake[i].parent as usize;
                    (sub(snake[i].rest, snake[parent].rest), sub(p, source[parent].ok_or("unmapped parent")?.2))
                }
            };
            let align = from_to(snake_dir, skater_dir);
            drive[i] = Some((s, mul(&transpose(&r), &align)));
        }
        Ok(Self { snake, drive, mirror })
    }

    /// Snake's bone world matrices for the skater pose (`root` and root-relative `bones`, both
    /// column-major 4x4 in metres), 16 floats each: rows X, Y, Z axis then translation, mm.
    pub fn pose(&self, root: &[f32; 16], bones: &[[f32; 16]]) -> Result<Vec<[f32; 16]>, String> {
        let (root_r, root_t) = split(root);
        let root_r = orthonormalize(&root_r);
        let skater = |s: usize| -> Result<(M3, V3), String> {
            let (r, p) = split(bones.get(s).ok_or_else(|| format!("skater pose has no bone {s}"))?);
            let r = orthonormalize(&r);
            Ok(if self.mirror { (mirror_rotation(&r), mirror_point(p)) } else { (r, p) })
        };
        let n = self.snake.len();
        let mut rot = vec![IDENTITY; n];
        let mut pos = vec![[0.; 3]; n];
        for i in 0..n {
            let parent = self.snake[i].parent;
            rot[i] = match self.drive[i] {
                Some((s, k)) => mul(&root_r, &mul(&skater(s)?.0, &k)),
                None if parent >= 0 => rot[parent as usize],
                None => root_r,
            };
            if parent >= 0 {
                let p = parent as usize;
                pos[i] = add(pos[p], apply(&rot[p], sub(self.snake[i].rest, self.snake[p].rest)));
            }
        }
        // Shift so Snake's ankle midpoint sits on the skater's (world, mm).
        let mut target = [0.; 3];
        let mut current = [0.; 3];
        for a in ANKLES {
            let (s, _) = self.drive[a].ok_or("ankle not mapped")?;
            let world = add(apply(&root_r, skater(s)?.1), root_t);
            target = add(target, scale(world, 500.));
            current = add(current, scale(pos[a], 0.5));
        }
        let shift = sub(target, current);
        Ok((0..n)
            .map(|i| {
                let [x, y, z] = rot[i];
                let t = add(pos[i], shift);
                let v = [x[0], x[1], x[2], 0., y[0], y[1], y[2], 0., z[0], z[1], z[2], 0., t[0], t[1], t[2], 1.];
                v.map(|f| f as f32)
            })
            .collect())
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn close(a: V3, b: V3, eps: f64) -> bool {
        length(sub(a, b)) < eps
    }

    fn close_m(a: &M3, b: &M3, eps: f64) -> bool {
        (0..3).all(|c| close(a[c], b[c], eps))
    }

    fn to16(r: &M3, t: V3) -> [f32; 16] {
        let v = [r[0][0], r[0][1], r[0][2], 0., r[1][0], r[1][1], r[1][2], 0., r[2][0], r[2][1], r[2][2], 0., t[0], t[1], t[2], 1.];
        v.map(|f| f as f32)
    }

    #[test]
    fn rotation_helpers() {
        let r = axis_angle(normalize([1., 2., 3.]), 0.7);
        assert!(close_m(&mul(&r, &transpose(&r)), &IDENTITY, 1e-12));
        assert!(close(apply(&axis_angle([0., 0., 1.], std::f64::consts::FRAC_PI_2), [1., 0., 0.]), [0., 1., 0.], 1e-12));
        for (a, b) in [([1., 0., 0.], [0., -1., 0.]), ([0.3, 1., -2.], [5., 0.1, 0.2]), ([0., 1., 0.], [0., -1., 0.]), ([1., 1., 1.], [2., 2., 2.])] {
            let m = from_to(a, b);
            assert!(close(apply(&m, normalize(a)), normalize(b), 1e-9), "{a:?} -> {b:?}");
            assert!(close_m(&mul(&m, &transpose(&m)), &IDENTITY, 1e-9));
        }
        let skewed = [scale(r[0], 1.01), scale(r[1], 0.99), scale(r[2], 1.02)];
        assert!(close_m(&orthonormalize(&skewed), &r, 1e-9));
        assert!(close_m(&mirror_rotation(&mirror_rotation(&r)), &r, 1e-15));
        let m = mirror_rotation(&r);
        assert!((dot(cross(m[0], m[1]), m[2]) - 1.).abs() < 1e-9, "mirrored frame stays a rotation");
    }

    /// A Snake-like skeleton covering every mapped bone, and a skater whose bind matches it
    /// exactly (bones point the same ways, arbitrary bind bases).
    fn synthetic() -> (Vec<SnakeBone>, Vec<String>, HashMap<String, (M3, V3)>) {
        let parents = [-1, 0, 1, 2, 3, 2, 5, 6, 7, 8, 2, 10, 11, 12, 13, 0, 15, 16, 17, 0, 19, 20, 21, 9];
        let rest: [V3; 24] = [
            [0., 0., 0.], [0., 30., 0.], [0., 220., 0.], [0., 440., 0.], [0., 560., 0.],
            [50., 400., -40.], [180., 400., -40.], [440., 400., -40.], [580., 400., -40.], [700., 400., -40.],
            [-50., 400., -40.], [-180., 400., -40.], [-440., 400., -40.], [-580., 400., -40.], [-700., 400., -40.],
            [100., -100., 30.], [100., -550., 30.], [100., -1000., 0.], [100., -1100., 100.],
            [-100., -100., 30.], [-100., -550., 30.], [-100., -1000., 0.], [-100., -1100., 100.],
            [790., 420., -15.],
        ];
        let snake: Vec<SnakeBone> = (0..24).map(|i| SnakeBone { parent: parents[i], rest: rest[i] }).collect();
        let mut names: Vec<String> = vec!["TRAJECTORY".into()];
        let mut bind = HashMap::new();
        for (k, (i, name)) in MAP.iter().enumerate() {
            if names.iter().any(|n| n == name) {
                continue;
            }
            names.push(name.to_string());
            let basis = axis_angle(normalize([1., k as f64, 2.]), 0.3 * k as f64);
            bind.insert(name.to_string(), (basis, scale(rest[*i], 0.001)));
        }
        (snake, names, bind)
    }

    #[test]
    fn rest_round_trip() {
        let (snake, names, bind) = synthetic();
        for mirror in [false, true] {
            let rt = Retarget::new(snake.clone(), &names, &bind, mirror).unwrap();
            // Skater posed exactly at bind, root at (2, 0.5, -3) m with identity rotation.
            let root = to16(&IDENTITY, [2., 0.5, -3.]);
            let bones: Vec<[f32; 16]> = names.iter().map(|n| bind.get(n).map_or(to16(&IDENTITY, [0.; 3]), |(r, p)| to16(r, *p))).collect();
            let out = rt.pose(&root, &bones).unwrap();
            for (i, m) in out.iter().enumerate() {
                let r: M3 = [0, 4, 8].map(|o| std::array::from_fn(|k| f64::from(m[o + k])));
                assert!(close_m(&r, &IDENTITY, 1e-5), "mirror {mirror} bone {i} rotation {r:?}");
                let t = [m[12], m[13], m[14]].map(f64::from);
                let want = add(snake[i].rest, [2000., 500., -3000.]);
                assert!(close(t, want, 0.05), "mirror {mirror} bone {i} at {t:?}, want {want:?}");
            }
        }
    }

    #[test]
    fn rotated_root_rotates_snake_and_keeps_lengths() {
        let (snake, names, bind) = synthetic();
        let rt = Retarget::new(snake.clone(), &names, &bind, false).unwrap();
        let turn = axis_angle([0., 1., 0.], 1.1);
        // Bend the left forearm 60 degrees about the bind's Z axis too.
        let bones: Vec<[f32; 16]> = names
            .iter()
            .map(|n| {
                let (r, p) = bind.get(n).copied().unwrap_or((IDENTITY, [0.; 3]));
                let r = if n == "LEFTFOREARM" { mul(&axis_angle([0., 0., 1.], 1.0), &r) } else { r };
                to16(&r, p)
            })
            .collect();
        let out = rt.pose(&to16(&turn, [0.; 3]), &bones).unwrap();
        let pos = |i: usize| [out[i][12], out[i][13], out[i][14]].map(f64::from);
        for (i, b) in snake.iter().enumerate().skip(1) {
            let want = length(sub(b.rest, snake[b.parent as usize].rest));
            assert!((length(sub(pos(i), pos(b.parent as usize))) - want).abs() < 0.01, "bone {i} length");
        }
        // Spine follows the root's turn.
        let spine = normalize(sub(pos(2), pos(1)));
        assert!(close(spine, [0., 1., 0.], 1e-5));
        let hip = normalize(sub(pos(15), pos(19)));
        assert!(close(hip, apply(&turn, normalize(sub(snake[15].rest, snake[19].rest))), 1e-5));
        // The forearm (and hand chain) bent upward by 60 degrees in the root frame.
        let forearm = normalize(sub(pos(9), pos(7)));
        assert!(close(forearm, apply(&turn, apply(&axis_angle([0., 0., 1.], 1.0), [1., 0., 0.])), 1e-5));
    }

    #[test]
    fn real_files_if_present() {
        let mdl = Path::new(r"C:\Program Files (x86)\Steam\steamapps\common\MGS3\assets\mdl\us\sna_def.mdl");
        let glb = Path::new(r"%USERPROFILE%\Games\Skate3-data\assets\private\skater.glb");
        let (Ok(data), true) = (std::fs::read(mdl), glb.exists()) else { return };
        let snake = read_mdl_bones(&data).unwrap();
        assert_eq!(snake.len(), 57);
        assert!(close(snake[9].rest, [700., 400., -40.], 1e-3));
        let bind = read_skater_bind(glb).unwrap();
        let names: Vec<String> = bind.keys().cloned().collect();
        let rt = Retarget::new(snake.clone(), &names, &bind, false).unwrap();
        // Skater at bind: every mapped Snake bone points along the skater's bind bone.
        let bones: Vec<[f32; 16]> = names.iter().map(|n| to16(&bind[n].0, bind[n].1)).collect();
        let out = rt.pose(&to16(&IDENTITY, [0.; 3]), &bones).unwrap();
        let pos = |i: usize| [out[i][12], out[i][13], out[i][14]].map(f64::from);
        let upper = normalize(sub(pos(7), pos(6)));
        let skater_upper = normalize(sub(bind["LEFTFOREARM"].1, bind["LEFTARM"].1));
        assert!(close(upper, skater_upper, 1e-4), "{upper:?} vs {skater_upper:?}");
        assert!(upper[1] < -0.6, "A-pose: arm hangs ~49 degrees");
        let shin = normalize(sub(pos(21), pos(20)));
        assert!(close(shin, normalize(sub(bind["RIGHTFOOT"].1, bind["RIGHTLEG"].1)), 1e-4));
    }
}
