//! MGS3 `.geom` collision reader: the solid triangles of a stage, in millimetres on the game's
//! own axes (Y up), wound counter-clockwise seen from the front.
//!
//! A port of `collect()` in `tools/geom.py` with its default options (no boxes, no loose
//! section-1 shapes); it must give the same triangles in the same order. The layout is in
//! `docs/formats.md`, ".geom (collision)". In short: grids (listed from 0x90) of cells, each
//! cell pointing at runs of group headers; solid groups (layer 0x1_) hold linked lists of
//! records: triangle meshes (0x2012), mesh boxes wrapping one (0x2004/0x2014) and instances
//! of section-1 shapes (0x2005).

use std::collections::{BTreeMap, BTreeSet, HashSet};

const REC_MESH: u16 = 0x2012;
const REC_INSTANCES: u16 = 0x2005;
const REC_MESH_BOX: [u16; 2] = [0x2004, 0x2014];
const LAYER_SOLID: u16 = 0x10;
const VERT_TAG: u16 = 1;

pub type Triangle = [[f32; 3]; 3];

/// A decoded file: triangles after removing exact repeats, the count before, and how many
/// times each kind of malformed data was skipped (geom.py's `problems`). The C API passes on
/// only the triangles; the tests check the rest.
#[cfg_attr(not(test), allow(dead_code))]
pub struct Decoded {
    pub triangles: Vec<Triangle>,
    /// Each triangle's face material word (`+0x0C` of its 0x2012 face; the first face kept
    /// when repeats are removed). Bit 0x0200_0000 marks MGS3's invisible walls.
    pub materials: Vec<u32>,
    pub raw: usize,
    pub problems: BTreeMap<String, usize>,
}

/// Instance transform: world = local · rows + position.
type Xform = ([[f64; 3]; 3], [f64; 3]);

struct Reader<'a> {
    d: &'a [u8],
    problems: BTreeMap<String, usize>,
}

pub fn decode(data: &[u8]) -> Result<Decoded, String> {
    let mut r = Reader { d: data, problems: BTreeMap::new() };
    let mut seen = HashSet::new();
    let (mut triangles, mut materials, mut raw) = (Vec::new(), Vec::new(), 0);
    for group in r.groups()? {
        if r.u16(group)? & 0xF0 != LAYER_SOLID {
            continue;
        }
        for (tri, material) in r.solid_triangles(group, None)? {
            raw += 1;
            // The same triangle is reached from several cells; geom.py keys on the
            // vertices rounded to 1 mm (half to even, -0 distinct from 0).
            let key = tri.map(|p| p.map(|c| c.round_ties_even().to_bits()));
            if seen.insert(key) {
                triangles.push(tri.map(|p| p.map(|c| c as f32)));
                materials.push(material);
            }
        }
    }
    Ok(Decoded { triangles, materials, raw, problems: r.problems })
}

impl Reader<'_> {
    fn bytes<const N: usize>(&self, o: usize) -> Result<[u8; N], String> {
        o.checked_add(N)
            .and_then(|end| self.d.get(o..end))
            .map(|b| b.try_into().unwrap())
            .ok_or_else(|| format!("read of {N} bytes at 0x{o:X} is past the end of the file"))
    }
    fn u16(&self, o: usize) -> Result<u16, String> {
        Ok(u16::from_le_bytes(self.bytes(o)?))
    }
    fn i16(&self, o: usize) -> Result<i16, String> {
        Ok(i16::from_le_bytes(self.bytes(o)?))
    }
    fn u32(&self, o: usize) -> Result<u32, String> {
        Ok(u32::from_le_bytes(self.bytes(o)?))
    }
    fn f32(&self, o: usize) -> Result<f64, String> {
        Ok(f32::from_le_bytes(self.bytes(o)?) as f64)
    }
    fn f32x3(&self, o: usize) -> Result<[f64; 3], String> {
        Ok([self.f32(o)?, self.f32(o + 4)?, self.f32(o + 8)?])
    }
    fn problem(&mut self, what: impl Into<String>) {
        *self.problems.entry(what.into()).or_default() += 1;
    }

    /// Item count in the high byte of a record's `+2`.
    fn count(&self, rec: usize) -> Result<usize, String> {
        Ok((self.u16(rec + 2)? >> 8) as usize)
    }

    /// Signed 28-bit record link at `o`, in 16-byte units (0 = end of list).
    fn link(&self, o: usize) -> Result<i64, String> {
        let v = (self.u32(o)? & 0x0FFF_FFFF) as i64;
        Ok(if v & (1 << 27) != 0 { v - (1 << 28) } else { v })
    }

    /// Every group header reached from the grids' cells: run heads plus their continuations,
    /// in file order.
    fn groups(&mut self) -> Result<Vec<usize>, String> {
        let mut heads = BTreeSet::new();
        let mut o = self.u32(0x18)? as usize;
        // Grid descriptors are 0x40 bytes; +0x20 = 1 marks the last.
        for _ in 0..256 {
            if o + 0x40 > self.d.len() {
                break;
            }
            let dims = [self.u16(o + 0x18)?, self.u16(o + 0x1A)?, self.u16(o + 0x1C)?];
            let (cells, table) = (self.u32(o + 0x30)? as usize, self.u32(o + 0x38)? as usize);
            if table != 0 {
                let n = dims.iter().map(|&n| n as usize).product::<usize>();
                for c in (0..n).map(|i| cells + 0x30 * i) {
                    let mask = self.u32(c)?;
                    for slot in 0..8 {
                        if mask >> slot & 1 != 0 {
                            heads.insert(table + self.u32(c + 0x10 + 4 * slot)? as usize);
                        }
                    }
                }
            }
            if self.u32(o + 0x20)? == 1 {
                break;
            }
            o += 0x40;
        }
        let mut all = BTreeSet::new();
        for head in heads {
            all.extend(self.run(head)?);
        }
        Ok(all.into_iter().collect())
    }

    /// The headers of the run starting at `head`: 0x30-byte headers, each followed by another
    /// while its type's low nibble is 0.
    fn run(&mut self, head: usize) -> Result<Vec<usize>, String> {
        let mut out = vec![head];
        while self.u16(*out.last().unwrap())? & 0x0F == 0 && out.len() < 4096 {
            let next = out.last().unwrap() + 0x30;
            if self.u32(next + 8)? != 0xFFFF_FFFF {
                self.problem("header run continues into a non-header");
                break;
            }
            out.push(next);
        }
        Ok(out)
    }

    /// Record offsets of a group's linked list (header `+0x20` = first record).
    fn chain(&mut self, group: usize) -> Result<Vec<usize>, String> {
        let mut out = Vec::new();
        let mut o = self.u32(group + 0x20)? as i64;
        for _ in 0..100_000 {
            if o < 0 || o as usize + 16 > self.d.len() {
                self.problem("record link out of file");
                break;
            }
            out.push(o as usize);
            match self.link(o as usize + 4)? {
                0 => break,
                step => o += step * 16,
            }
        }
        Ok(out)
    }

    /// World triangles of one solid group, following instances one level deep.
    fn solid_triangles(&mut self, group: usize, xform: Option<&Xform>) -> Result<Vec<([[f64; 3]; 3], u32)>, String> {
        let mut out = Vec::new();
        for mut rec in self.chain(group)? {
            let mut kind = self.u16(rec)?;
            if REC_MESH_BOX.contains(&kind) {
                // Child link in 16-byte units at +0x0C; 0 = empty box.
                let child = self.u32(rec + 0x0C)? as usize;
                if child == 0 {
                    continue;
                }
                rec += 16 * child;
                kind = self.u16(rec)?;
                if kind != REC_MESH {
                    self.problem(format!("mesh box child is 0x{kind:04X}"));
                    continue;
                }
            }
            if kind == REC_MESH {
                let mesh = self.mesh(rec)?;
                out.extend(mesh.into_iter().map(|(tri, material)| match xform {
                    Some(x) => (tri.map(|p| transform(x, p)), material),
                    None => (tri, material),
                }));
            } else if kind == REC_INSTANCES && xform.is_none() {
                for (x, shape) in self.instances(rec)? {
                    for group in self.run(shape)? {
                        out.extend(self.solid_triangles(group, Some(&x))?);
                    }
                }
            } else {
                self.problem(format!("unknown solid record 0x{kind:04X}"));
            }
        }
        Ok(out)
    }

    /// 0x2012: 0x30-byte faces from +0x20, each with six u32 offsets relative to the face at
    /// +0x10 (v0..v3, normal, pool origin). v3 == v0 marks a triangle, else a planar quad.
    fn mesh(&mut self, rec: usize) -> Result<Vec<([[f64; 3]; 3], u32)>, String> {
        let mut out = Vec::new();
        for j in 0..self.count(rec)? {
            let s = rec + 0x20 + 0x30 * j;
            let mut offs = [0usize; 6];
            for (i, off) in offs.iter_mut().enumerate() {
                *off = self.u32(s + 0x10 + 4 * i)? as usize;
            }
            if s + offs.iter().max().unwrap() + 16 > self.d.len() {
                self.problem("triangle offset out of file");
                continue;
            }
            let quad = offs[3] != offs[0];
            let material = self.u32(s + 0x0C)?;
            let origin = self.f32x3(s + offs[5])?;
            let mut p = [[0f64; 3]; 4];
            let mut untagged = false;
            for (i, v) in p.iter_mut().enumerate().take(if quad { 4 } else { 3 }) {
                // 8-byte unit: i16 x, y, z (mm from the origin) + u16 tag.
                let o = s + offs[i];
                let xyz = [self.i16(o)?, self.i16(o + 2)?, self.i16(o + 4)?];
                let tag = self.u16(o + 6)?;
                // One s162a0 vertex is an all-zero unit (tag 0), used as (0, 0, 0).
                untagged |= tag != VERT_TAG && (tag != 0 || xyz != [0; 3]);
                *v = [0, 1, 2].map(|k| xyz[k] as f64 + origin[k]);
            }
            if untagged {
                self.problem("face vertex not tagged 1");
            }
            out.push(([p[0], p[1], p[2]], material));
            if quad {
                out.push(([p[0], p[2], p[3]], material));
            }
        }
        Ok(out)
    }

    /// 0x2005: 0x90-byte entries from +0x20: rotation rows (float4) at +0x30, position at
    /// +0x60, section-1 run head at +0x70.
    fn instances(&self, rec: usize) -> Result<Vec<(Xform, usize)>, String> {
        (0..self.count(rec)?)
            .map(|j| {
                let e = rec + 0x20 + 0x90 * j;
                let rows = [self.f32x3(e + 0x30)?, self.f32x3(e + 0x40)?, self.f32x3(e + 0x50)?];
                Ok(((rows, self.f32x3(e + 0x60)?), self.u32(e + 0x70)? as usize))
            })
            .collect()
    }
}

fn transform((rows, pos): &Xform, p: [f64; 3]) -> [f64; 3] {
    [0, 1, 2].map(|k| p[0] * rows[0][k] + p[1] * rows[1][k] + p[2] * rows[2][k] + pos[k])
}

#[cfg(test)]
mod tests {
    use super::*;

    /// A one-grid, one-cell file with a single solid group holding one triangle mesh.
    #[test]
    fn decodes_one_triangle() {
        let mut d = vec![0u8; 0x200];
        let put = |d: &mut Vec<u8>, o: usize, b: &[u8]| d[o..o + b.len()].copy_from_slice(b);
        put(&mut d, 0x18, &0x90u32.to_le_bytes());
        // Grid: 1x1x1 cells at 0xD0, header table at 0x100, last grid.
        put(&mut d, 0x90 + 0x18, &[1, 0, 1, 0, 1, 0]);
        put(&mut d, 0x90 + 0x20, &1u32.to_le_bytes());
        put(&mut d, 0x90 + 0x30, &0xD0u32.to_le_bytes());
        put(&mut d, 0x90 + 0x38, &0x100u32.to_le_bytes());
        put(&mut d, 0xD0, &2u32.to_le_bytes()); // slot 5
        put(&mut d, 0xD0 + 0x14, &0u32.to_le_bytes());
        // Header: layer 0x11 (last of run), first record at 0x140.
        put(&mut d, 0x100, &0x11u16.to_le_bytes());
        put(&mut d, 0x108, &u32::MAX.to_le_bytes());
        put(&mut d, 0x120, &0x140u32.to_le_bytes());
        // Mesh with one face at 0x160; pool at 0x190 (three vertices, then origin).
        put(&mut d, 0x140, &REC_MESH.to_le_bytes());
        put(&mut d, 0x142, &0x100u16.to_le_bytes());
        for (i, off) in [0x30u32, 0x38, 0x40, 0x30, 0, 0x48].iter().enumerate() {
            put(&mut d, 0x170 + 4 * i, &off.to_le_bytes());
        }
        for (i, v) in [[0i16, 0, 0], [10, 0, 0], [0, 0, -10]].iter().enumerate() {
            for (k, c) in v.iter().enumerate() {
                put(&mut d, 0x190 + 8 * i + 2 * k, &c.to_le_bytes());
            }
            put(&mut d, 0x190 + 8 * i + 6, &1u16.to_le_bytes());
        }
        put(&mut d, 0x1A8, &[100f32.to_le_bytes(), 5f32.to_le_bytes(), 0f32.to_le_bytes()].concat());

        let g = decode(&d).unwrap();
        assert_eq!(g.raw, 1);
        assert!(g.problems.is_empty(), "{:?}", g.problems);
        assert_eq!(g.triangles, vec![[[100., 5., 0.], [110., 5., 0.], [100., 5., -10.]]]);
    }

    /// The stages tools/geom_compare.py checks against geom.py, read from the game install if
    /// it is there: nothing skipped, and repeats only ever removed.
    #[test]
    fn game_files_decode_cleanly() {
        let dir = std::path::Path::new(r"C:\Program Files (x86)\Steam\steamapps\common\MGS3\assets\geom\us");
        for stage in ["c001b", "s091a", "s121a", "s003a", "v004a", "s201a"] {
            let Ok(data) = std::fs::read(dir.join(format!("{stage}.geom"))) else {
                continue;
            };
            let g = decode(&data).unwrap();
            assert!(g.problems.is_empty(), "{stage}: {:?}", g.problems);
            assert!(!g.triangles.is_empty() && g.raw >= g.triangles.len(), "{stage}");
        }
    }

    #[test]
    fn truncated_file_is_an_error() {
        assert!(decode(&[0u8; 8]).is_err());
    }
}
