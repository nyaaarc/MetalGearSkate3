//! Floor lookup over the installed collision: the highest upward-facing surface straight below a
//! point. The plugin uses it to catch the skater leaving the stage before MGS3 sees Snake fall.

use std::collections::HashMap;

/// Grid cell size, engine units (metres).
const CELL: f32 = 2.;
/// Surfaces steeper than this (|normal y| below it) are walls, not floor (either winding).
const MIN_NORMAL_Y: f32 = 0.3;

pub struct Floor {
    triangles: Vec<[[f32; 3]; 3]>,
    cells: HashMap<(i32, i32), Vec<u32>>,
}

fn cell(v: f32) -> i32 {
    (v / CELL).floor() as i32
}

impl Floor {
    pub fn new(triangles: &[[[f32; 3]; 3]]) -> Self {
        let mut floor = Self { triangles: Vec::new(), cells: HashMap::new() };
        for t in triangles {
            let (e1, e2) = (sub(t[1], t[0]), sub(t[2], t[0]));
            let n = cross(e1, e2);
            let len = (n[0] * n[0] + n[1] * n[1] + n[2] * n[2]).sqrt();
            if !(len > 1e-9) || n[1].abs() / len < MIN_NORMAL_Y {
                continue;
            }
            let index = floor.triangles.len() as u32;
            floor.triangles.push(*t);
            let (x0, x1) = (t.iter().map(|p| p[0]).fold(f32::MAX, f32::min), t.iter().map(|p| p[0]).fold(f32::MIN, f32::max));
            let (z0, z1) = (t.iter().map(|p| p[2]).fold(f32::MAX, f32::min), t.iter().map(|p| p[2]).fold(f32::MIN, f32::max));
            for cx in cell(x0)..=cell(x1) {
                for cz in cell(z0)..=cell(z1) {
                    floor.cells.entry((cx, cz)).or_default().push(index);
                }
            }
        }
        floor
    }

    /// Height of the highest floor at (x, z) no higher than `y`, if any.
    pub fn below(&self, x: f32, y: f32, z: f32) -> Option<f32> {
        let mut best: Option<f32> = None;
        for &i in self.cells.get(&(cell(x), cell(z)))? {
            let t = &self.triangles[i as usize];
            // Barycentric test in the xz plane, then the plane's height there.
            let (ax, az, bx, bz, cx, cz) = (t[0][0], t[0][2], t[1][0], t[1][2], t[2][0], t[2][2]);
            let det = (bz - cz) * (ax - cx) + (cx - bx) * (az - cz);
            if det.abs() < 1e-12 {
                continue;
            }
            let u = ((bz - cz) * (x - cx) + (cx - bx) * (z - cz)) / det;
            let v = ((cz - az) * (x - cx) + (ax - cx) * (z - cz)) / det;
            let w = 1. - u - v;
            const EPS: f32 = -1e-4;
            if u < EPS || v < EPS || w < EPS {
                continue;
            }
            let h = u * t[0][1] + v * t[1][1] + w * t[2][1];
            if h <= y && best.is_none_or(|b| h > b) {
                best = Some(h);
            }
        }
        best
    }
}

fn sub(a: [f32; 3], b: [f32; 3]) -> [f32; 3] {
    [a[0] - b[0], a[1] - b[1], a[2] - b[2]]
}

fn cross(a: [f32; 3], b: [f32; 3]) -> [f32; 3] {
    [a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]]
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn finds_the_floor_under_a_point_and_nothing_off_it() {
        let quad = [[[0., 1., 0.], [0., 1., 4.], [4., 1., 0.]], [[4., 1., 0.], [0., 1., 4.], [4., 1., 4.]]];
        let wall = [[[0., 0., 0.], [0., 5., 0.], [0., 0., 4.]]];
        let floor = Floor::new(&[quad[0], quad[1], wall[0]]);
        assert_eq!(floor.below(1., 3., 1.), Some(1.));
        assert_eq!(floor.below(3., 3., 3.5), Some(1.));
        assert_eq!(floor.below(1., 0.5, 1.), None);
        assert_eq!(floor.below(5., 3., 1.), None);
    }
}
