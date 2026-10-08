//! Grind rails found in a stage's collision: the lips a skater can grind.
//!
//! Ported from `crates/render_anim/src/skate/rails.rs` in chasmlol's
//! [2010 Rust Rewrite Mashup](https://github.com/chasmlol/2010-rust-rewrite-mashup)
//! (Apache-2.0; see `mashup/LICENSE`). Changes: input is engine metres with +Y up instead of
//! MW2 inches with +Z up, thresholds are restated in metres and tuned for MGS3, and thin
//! boxes (railings, pipes, beams) yield one rail on their top centre instead of two lips.
//!
//! MGS3 collision is a soup of meshes and boxes, so the top of a ledge and its wall rarely
//! share an edge vertex for vertex. A lip is therefore found by probing the geometry around
//! each edge of a walkable face rather than by matching triangles: the ground must fall away
//! just past the edge and nothing may rise there. Surviving edges are merged along their lines
//! across seams and T-junctions, then chained into polylines where they meet at a gentle turn.

use std::collections::HashMap;

use glam::Vec3;

const UP: Vec3 = Vec3::Y;
/// A face this upward is walkable; its edges are rail candidates.
const UPWARD: f32 = 0.65;
/// Horizontal grid cell for probing.
const CELL: f32 = 2.0;
/// How high above the lip the drop probe starts.
const PROBE_LIFT: f32 = 0.03;
/// How far past the lip the probes look.
const PROBE_OUT: f32 = 0.05;
/// Open space the ground must drop below the lip, just past it: a kerb is about 0.1 m.
const MIN_DROP: f32 = 0.08;
/// Height above the lip the wall probe runs at.
const WALL_PROBE_UP: f32 = 0.08;
/// How far behind the lip the wall probe starts.
const WALL_PROBE_BACK: f32 = 0.03;
/// Edges shorter than this are ignored.
const MIN_EDGE: f32 = 0.03;
/// Edges shorter than this are probed once, at their middle.
const SHORT_EDGE: f32 = 0.3;
/// Shortest rail kept, after merging and chaining.
const MIN_RAIL: f32 = 0.75;
/// Steepest rail, as rise over length: stair handrails and ramp edges pass.
const MAX_SLOPE: f32 = 0.7;
/// Chains continue through a joint turning less than about 35 degrees.
const MIN_TURN_COS: f32 = 0.82;
/// Collinear pieces closer than this along their line are one run.
const MERGE_GAP: f32 = 0.04;
/// Joints closer than this are one point.
const NODE: f32 = 0.025;
/// Two parallel lips at most this far apart, at the same height and overlapping, are the
/// two top edges of one thin box (a railing or pipe) and become one rail between them.
const THIN_WIDTH: f32 = 0.35;
const THIN_HEIGHT: f32 = 0.03;
/// The skate engine counts rails in a u16.
const MAX_RAILS: usize = u16::MAX as usize;

#[derive(Default, Debug, Clone, Copy)]
pub struct Census {
    /// Edges of walkable faces.
    pub candidates: usize,
    /// Of those, edges the ground drops away from.
    pub lips: usize,
    /// Lips merged along their lines.
    pub runs: usize,
    /// Runs that were pairs of thin-box top edges, joined into one.
    pub thin: usize,
    /// Rails kept.
    pub rails: usize,
}

/// Rails over `tris` (metres, +Y up), each a polyline of two or more points.
pub fn find(tris: &[[[f32; 3]; 3]]) -> (Vec<Vec<[f32; 3]>>, Census) {
    let tris: Vec<[Vec3; 3]> = tris.iter().map(|t| t.map(Vec3::from_array)).collect();
    let grid = Grid::build(&tris);
    let mut probe = Probe { tris: &tris, grid: &grid, stamp: vec![0; tris.len()], round: 0, scratch: Vec::new() };

    // Every edge of a walkable face, once.
    let key = |v: Vec3| v.to_array().map(|x| (x * 300.).round() as i32);
    let mut edges: HashMap<([i32; 3], [i32; 3]), (Vec3, Vec3, Vec3, Vec3)> = HashMap::new();
    for tri in &tris {
        let normal = (tri[1] - tri[0]).cross(tri[2] - tri[0]).normalize_or_zero();
        if normal.dot(UP) <= UPWARD {
            continue;
        }
        let centroid = (tri[0] + tri[1] + tri[2]) / 3.;
        for i in 0..3 {
            let (a, b) = (tri[i], tri[(i + 1) % 3]);
            let (ka, kb) = (key(a), key(b));
            let k = if ka < kb { (ka, kb) } else { (kb, ka) };
            edges.entry(k).or_insert((a, b, normal, centroid));
        }
    }
    let mut census = Census { candidates: edges.len(), ..Census::default() };

    let mut lips = Vec::new();
    for (a, b, normal, centroid) in edges.into_values() {
        let along = b - a;
        let len = along.length();
        if len < MIN_EDGE || along.dot(UP).abs() > MAX_SLOPE * len {
            continue;
        }
        let mid = (a + b) * 0.5;
        let mut out = along.cross(normal).normalize_or_zero();
        if out.dot(centroid - mid) > 0. {
            out = -out;
        }
        let samples: &[f32] = if len < SHORT_EDGE { &[0.5] } else { &[0.25, 0.5, 0.75] };
        let passing = samples.iter().filter(|&&s| probe.is_lip(a + along * s, out)).count();
        if passing * 3 >= samples.len() * 2 {
            lips.push((a, b));
        }
    }
    census.lips = lips.len();

    let runs = merge_collinear(lips);
    census.runs = runs.len();
    let (runs, thin) = join_thin(runs);
    census.thin = thin;
    let mut rails = chain(runs);
    rails.retain(|rail| polyline_len(rail) >= MIN_RAIL);
    if rails.len() > MAX_RAILS {
        rails.sort_by(|a, b| polyline_len(b).total_cmp(&polyline_len(a)));
        rails.truncate(MAX_RAILS);
    }
    census.rails = rails.len();
    (rails.into_iter().map(|r| r.into_iter().map(Vec3::to_array).collect()).collect(), census)
}

fn polyline_len(points: &[Vec3]) -> f32 {
    points.windows(2).map(|w| w[0].distance(w[1])).sum()
}

/// Triangles bucketed by the horizontal (X, Z) cells their bounds cover.
struct Grid {
    cells: HashMap<(i32, i32), Vec<u32>>,
}

fn cell_of(v: f32) -> i32 {
    (v / CELL).floor() as i32
}

impl Grid {
    fn build(tris: &[[Vec3; 3]]) -> Self {
        let mut cells: HashMap<(i32, i32), Vec<u32>> = HashMap::new();
        for (index, tri) in tris.iter().enumerate() {
            let min = tri[0].min(tri[1]).min(tri[2]);
            let max = tri[0].max(tri[1]).max(tri[2]);
            for x in cell_of(min.x)..=cell_of(max.x) {
                for z in cell_of(min.z)..=cell_of(max.z) {
                    cells.entry((x, z)).or_default().push(index as u32);
                }
            }
        }
        Self { cells }
    }
}

struct Probe<'a> {
    tris: &'a [[Vec3; 3]],
    grid: &'a Grid,
    stamp: Vec<u32>,
    round: u32,
    scratch: Vec<u32>,
}

impl Probe<'_> {
    /// Past `p` along `out` the ground falls away and nothing rises.
    fn is_lip(&mut self, p: Vec3, out: Vec3) -> bool {
        let down_from = p + out * PROBE_OUT + UP * PROBE_LIFT;
        if self.hit(down_from, -UP, PROBE_LIFT + MIN_DROP) {
            return false;
        }
        let across_from = p - out * WALL_PROBE_BACK + UP * WALL_PROBE_UP;
        !self.hit(across_from, out, WALL_PROBE_BACK + PROBE_OUT)
    }

    /// Whether the segment from `origin` along unit `dir` for `len` crosses any triangle,
    /// either side facing.
    fn hit(&mut self, origin: Vec3, dir: Vec3, len: f32) -> bool {
        let end = origin + dir * len;
        let (min, max) = (origin.min(end), origin.max(end));
        self.round = self.round.wrapping_add(1);
        if self.round == 0 {
            self.stamp.fill(0);
            self.round = 1;
        }
        self.scratch.clear();
        for x in cell_of(min.x)..=cell_of(max.x) {
            for z in cell_of(min.z)..=cell_of(max.z) {
                let Some(cell) = self.grid.cells.get(&(x, z)) else {
                    continue;
                };
                for &index in cell {
                    let seen = &mut self.stamp[index as usize];
                    if *seen != self.round {
                        *seen = self.round;
                        self.scratch.push(index);
                    }
                }
            }
        }
        self.scratch.iter().any(|&index| ray_triangle(origin, dir, len, &self.tris[index as usize]))
    }
}

/// Möller–Trumbore, both faces, hits within `(0, len]`.
fn ray_triangle(origin: Vec3, dir: Vec3, len: f32, tri: &[Vec3; 3]) -> bool {
    let e1 = tri[1] - tri[0];
    let e2 = tri[2] - tri[0];
    let p = dir.cross(e2);
    let det = e1.dot(p);
    if det.abs() < 1e-10 {
        return false;
    }
    let inv = 1. / det;
    let s = origin - tri[0];
    let u = s.dot(p) * inv;
    if !(0. ..=1.).contains(&u) {
        return false;
    }
    let q = s.cross(e1);
    let v = dir.dot(q) * inv;
    if v < 0. || u + v > 1. {
        return false;
    }
    let t = e2.dot(q) * inv;
    t > 1e-5 && t <= len
}

/// `d` pointing the canonical way along its line (+X, then +Z, then +Y).
fn canonical(d: Vec3) -> bool {
    let e = 1e-4;
    !(d.x < -e || (d.x.abs() <= e && (d.z < -e || (d.z.abs() <= e && d.y < 0.))))
}

/// Lips on one line, merged into maximal runs where they overlap or touch.
fn merge_collinear(lips: Vec<(Vec3, Vec3)>) -> Vec<(Vec3, Vec3)> {
    let mut lines: HashMap<([i32; 3], [i32; 3]), (Vec3, Vec3, Vec<(f32, f32)>)> = HashMap::new();
    for (mut a, mut b) in lips {
        let mut d = (b - a).normalize_or_zero();
        if d == Vec3::ZERO {
            continue;
        }
        if !canonical(d) {
            d = -d;
            std::mem::swap(&mut a, &mut b);
        }
        let o = a - d * a.dot(d);
        let k = (
            (d * 64.).to_array().map(|x| x.round() as i32),
            (o * 80.).to_array().map(|x| x.round() as i32),
        );
        let line = lines.entry(k).or_insert((d, o, Vec::new()));
        line.2.push((a.dot(line.0), b.dot(line.0)));
    }
    let mut runs = Vec::new();
    for (d, o, mut spans) in lines.into_values() {
        spans.sort_by(|x, y| x.0.total_cmp(&y.0));
        let mut current = spans[0];
        for &(t0, t1) in &spans[1..] {
            if t0 <= current.1 + MERGE_GAP {
                current.1 = current.1.max(t1);
            } else {
                runs.push((o + d * current.0, o + d * current.1));
                current = (t0, t1);
            }
        }
        runs.push((o + d * current.0, o + d * current.1));
    }
    runs
}

/// Pairs of parallel runs at one height, overlapping along their length and at most
/// `THIN_WIDTH` apart across it with nothing walkable between them but the box top, are the
/// two lips of a thin box: each pair becomes one run on the centre line over their overlap.
/// Returns the runs and how many pairs were joined.
fn join_thin(mut runs: Vec<(Vec3, Vec3)>) -> (Vec<(Vec3, Vec3)>, usize) {
    for run in &mut runs {
        if !canonical(run.1 - run.0) {
            *run = (run.1, run.0);
        }
    }
    // Bucket by direction and horizontal position so the pairing stays near linear.
    let bucket = |a: Vec3| ((a.x / 4.).floor() as i32, (a.z / 4.).floor() as i32);
    let mut near: HashMap<(i32, i32), Vec<usize>> = HashMap::new();
    for (i, (a, b)) in runs.iter().enumerate() {
        let m = (*a + *b) * 0.5;
        near.entry(bucket(m)).or_default().push(i);
    }
    let mut taken = vec![false; runs.len()];
    let mut joined = Vec::new();
    // Longest first, so a long rail claims its partner before a stub does.
    let mut order: Vec<usize> = (0..runs.len()).collect();
    order.sort_by(|&x, &y| runs[y].0.distance(runs[y].1).total_cmp(&runs[x].0.distance(runs[x].1)));
    for &i in &order {
        if taken[i] {
            continue;
        }
        let (a, b) = runs[i];
        let len = a.distance(b);
        let d = (b - a) / len;
        let m = (a + b) * 0.5;
        let (bx, bz) = bucket(m);
        let mut best: Option<(usize, f32)> = None;
        for x in bx - 1..=bx + 1 {
            for z in bz - 1..=bz + 1 {
                for &j in near.get(&(x, z)).into_iter().flatten() {
                    if j == i || taken[j] {
                        continue;
                    }
                    let (c, e) = runs[j];
                    let dj = (e - c).normalize_or_zero();
                    if d.dot(dj) < 0.995 {
                        continue;
                    }
                    let offset = (c - a) - d * (c - a).dot(d);
                    if offset.dot(UP).abs() > THIN_HEIGHT {
                        continue;
                    }
                    let width = offset.length();
                    if width > THIN_WIDTH || width < 0.01 {
                        continue;
                    }
                    let (t0, t1) = ((c - a).dot(d), (e - a).dot(d));
                    let overlap = t1.min(len) - t0.max(0.);
                    let shorter = len.min(t1 - t0);
                    if overlap < 0.5 * shorter {
                        continue;
                    }
                    if best.is_none_or(|(_, w)| width < w) {
                        best = Some((j, width));
                    }
                }
            }
        }
        if let Some((j, _)) = best {
            let (c, e) = runs[j];
            let offset = (c - a) - d * (c - a).dot(d);
            let (t0, t1) = ((c - a).dot(d).max(0.), (e - a).dot(d).min(len));
            let centre = a + offset * 0.5;
            taken[i] = true;
            taken[j] = true;
            joined.push((centre + d * t0, centre + d * t1));
        }
    }
    let pairs = joined.len();
    let mut out: Vec<(Vec3, Vec3)> = runs.into_iter().zip(taken).filter(|(_, t)| !t).map(|(r, _)| r).collect();
    out.extend(joined);
    (out, pairs)
}

/// Runs joined end to end into polylines through joints where exactly two runs meet at a
/// gentle turn.
fn chain(runs: Vec<(Vec3, Vec3)>) -> Vec<Vec<Vec3>> {
    let node = |v: Vec3| v.to_array().map(|x| (x / NODE).round() as i32);
    let mut at: HashMap<[i32; 3], Vec<(usize, bool)>> = HashMap::new();
    for (i, (a, b)) in runs.iter().enumerate() {
        at.entry(node(*a)).or_default().push((i, false));
        at.entry(node(*b)).or_default().push((i, true));
    }
    let mut used = vec![false; runs.len()];
    let far = |i: usize, from_end: bool| if from_end { runs[i].0 } else { runs[i].1 };
    let near = |i: usize, from_end: bool| if from_end { runs[i].1 } else { runs[i].0 };
    // The run continuing a polyline at `joint`, arriving along `heading`.
    let next = |joint: Vec3, heading: Vec3, used: &[bool]| -> Option<(usize, bool)> {
        let there = at.get(&node(joint))?;
        if there.len() != 2 {
            return None;
        }
        let &(i, at_end) = there.iter().find(|(i, _)| !used[*i])?;
        let leave = (far(i, at_end) - near(i, at_end)).normalize_or_zero();
        (heading.dot(leave) >= MIN_TURN_COS).then_some((i, at_end))
    };
    let mut rails = Vec::new();
    for start in 0..runs.len() {
        if used[start] {
            continue;
        }
        used[start] = true;
        let (a, b) = runs[start];
        let mut points = std::collections::VecDeque::from([a, b]);
        // Forward from b, then backward from a.
        let mut tip = b;
        let mut heading = (b - a).normalize_or_zero();
        while let Some((i, at_end)) = next(tip, heading, &used) {
            used[i] = true;
            let to = far(i, at_end);
            heading = (to - tip).normalize_or_zero();
            tip = to;
            points.push_back(to);
        }
        let mut tip = a;
        let mut heading = (a - b).normalize_or_zero();
        while let Some((i, at_end)) = next(tip, heading, &used) {
            used[i] = true;
            let to = far(i, at_end);
            heading = (to - tip).normalize_or_zero();
            tip = to;
            points.push_front(to);
        }
        rails.push(points.into_iter().collect());
    }
    rails
}

#[cfg(test)]
mod tests {
    use super::*;

    fn quad(a: [f32; 3], b: [f32; 3], c: [f32; 3], d: [f32; 3]) -> [[[f32; 3]; 3]; 2] {
        [[a, b, c], [a, c, d]]
    }

    /// A 1 m x 0.4 m x 4 m box (top at y = 0.4) on a floor: its four top edges are lips.
    #[test]
    fn box_on_floor_has_top_edges() {
        let mut tris = Vec::new();
        tris.extend(quad([-10., 0., -10.], [-10., 0., 10.], [10., 0., 10.], [10., 0., -10.]));
        let (x0, x1, z0, z1, h) = (-0.5, 0.5, -2., 2., 0.4);
        tris.extend(quad([x0, h, z0], [x0, h, z1], [x1, h, z1], [x1, h, z0]));
        tris.extend(quad([x0, 0., z0], [x0, h, z0], [x1, h, z0], [x1, 0., z0]));
        tris.extend(quad([x0, 0., z1], [x1, 0., z1], [x1, h, z1], [x0, h, z1]));
        tris.extend(quad([x0, 0., z0], [x0, 0., z1], [x0, h, z1], [x0, h, z0]));
        tris.extend(quad([x1, 0., z0], [x1, h, z0], [x1, h, z1], [x1, 0., z1]));
        let (rails, census) = find(&tris);
        assert_eq!(census.rails, rails.len());
        // The floor's own outer edges are lips too; keep the box's. Its four top edges turn
        // 90 degrees at each corner, so they stay four rails.
        let rails: Vec<_> = rails.into_iter().filter(|r| r.iter().all(|p| (p[1] - h).abs() < 1e-4)).collect();
        assert_eq!(rails.len(), 4, "{rails:?}");
        let total: f32 = rails.iter().map(|r| polyline_len(&r.iter().copied().map(Vec3::from_array).collect::<Vec<_>>())).sum();
        assert!((total - 10.).abs() < 0.01, "total {total}, {rails:?}");
    }
}
