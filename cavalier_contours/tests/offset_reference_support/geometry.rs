//! Independent double precision geometry used as the reference oracle.
//!
//! This module only reads input vertex data (positions and bulges) from polylines. It stores its
//! own directed line segments and circular arcs and computes distances, horizontal ray crossings,
//! winding numbers and signed areas from those primitives. It deliberately does not call any
//! library geometry routine (`closest_point`, `winding_number`, `seg_arc_radius_and_center`,
//! `area`, offset internals) when producing expected values.

#![allow(dead_code)]

use cavalier_contours::polyline::{PlineSource, PlineVertex, Polyline};

use std::f64::consts::{PI, TAU};

/// Tolerance used only to decide whether a polar angle lies on a finite arc sweep. Positions are
/// compared analytically elsewhere; this is not a probe skip band.
pub const ARC_ANGLE_EPS: f64 = 1e-9;

/// A 2D point / vector in double precision.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Pt {
    pub x: f64,
    pub y: f64,
}

impl Pt {
    #[inline]
    pub fn new(x: f64, y: f64) -> Self {
        Self { x, y }
    }

    #[inline]
    pub fn add(self, rhs: Self) -> Self {
        Self::new(self.x + rhs.x, self.y + rhs.y)
    }

    #[inline]
    pub fn sub(self, rhs: Self) -> Self {
        Self::new(self.x - rhs.x, self.y - rhs.y)
    }

    #[inline]
    pub fn scale(self, s: f64) -> Self {
        Self::new(self.x * s, self.y * s)
    }

    #[inline]
    pub fn dot(self, rhs: Self) -> f64 {
        self.x * rhs.x + self.y * rhs.y
    }

    #[inline]
    pub fn perp_dot(self, rhs: Self) -> f64 {
        self.x * rhs.y - self.y * rhs.x
    }

    #[inline]
    pub fn perp(self) -> Self {
        Self::new(-self.y, self.x)
    }

    #[inline]
    pub fn length(self) -> f64 {
        self.dot(self).sqrt()
    }

    #[inline]
    pub fn distance(self, rhs: Self) -> f64 {
        self.sub(rhs).length()
    }
}

/// A directed finite circular arc.
#[derive(Clone, Copy, Debug)]
pub struct RefArc {
    pub start: Pt,
    pub end: Pt,
    pub center: Pt,
    pub radius: f64,
    /// Polar angle of `start` around `center`.
    pub start_angle: f64,
    /// Signed sweep angle, positive for counter clockwise. Always |sweep| <= PI.
    pub sweep: f64,
}

impl RefArc {
    /// Point at normalized parameter `t` in `[0, 1]`.
    pub fn point_at(&self, t: f64) -> Pt {
        let angle = self.start_angle + self.sweep * t;
        self.center
            .add(Pt::new(angle.cos(), angle.sin()).scale(self.radius))
    }

    /// Unit tangent at normalized parameter `t`.
    pub fn tangent_at(&self, t: f64) -> Pt {
        let angle = self.start_angle + self.sweep * t;
        let dir = Pt::new(-angle.sin(), angle.cos());
        if self.sweep < 0.0 {
            dir.scale(-1.0)
        } else {
            dir
        }
    }

    /// Distance from `p` to the finite arc.
    pub fn distance(&self, p: Pt) -> f64 {
        let rel = p.sub(self.center);
        let radial = rel.length();
        if radial <= f64::EPSILON {
            // All arc points are exactly `radius` away from the center.
            return self.radius.abs();
        }
        let angle = rel.y.atan2(rel.x);
        if self.angle_param(angle, ARC_ANGLE_EPS).is_some() {
            (radial - self.radius).abs()
        } else {
            p.distance(self.start).min(p.distance(self.end))
        }
    }

    /// Parameter of polar `angle` on the finite sweep, or `None` if outside.
    pub fn angle_param(&self, angle: f64, tol: f64) -> Option<f64> {
        let abs_sweep = self.sweep.abs();
        if abs_sweep <= tol {
            return None;
        }
        let delta = if self.sweep >= 0.0 {
            (angle - self.start_angle).rem_euclid(TAU)
        } else {
            (self.start_angle - angle).rem_euclid(TAU)
        };
        if delta > abs_sweep + tol {
            return None;
        }
        Some((delta / abs_sweep).clamp(0.0, 1.0))
    }
}

/// A directed finite segment, either straight or circular.
#[derive(Clone, Copy, Debug)]
pub enum RefSeg {
    Line { start: Pt, end: Pt },
    Arc(RefArc),
}

impl RefSeg {
    #[inline]
    pub fn start(&self) -> Pt {
        match *self {
            Self::Line { start, .. } => start,
            Self::Arc(arc) => arc.start,
        }
    }

    #[inline]
    pub fn end(&self) -> Pt {
        match *self {
            Self::Line { end, .. } => end,
            Self::Arc(arc) => arc.end,
        }
    }

    /// Point at normalized parameter `t` in `[0, 1]`.
    pub fn point_at(&self, t: f64) -> Pt {
        match *self {
            Self::Line { start, end } => start.add(end.sub(start).scale(t)),
            Self::Arc(arc) => arc.point_at(t),
        }
    }

    /// Unit tangent at normalized parameter `t` in `[0, 1]`.
    pub fn tangent_at(&self, t: f64) -> Pt {
        match *self {
            Self::Line { start, end } => {
                let d = end.sub(start);
                let len = d.length();
                if len <= f64::EPSILON {
                    Pt::new(0.0, 0.0)
                } else {
                    d.scale(1.0 / len)
                }
            }
            Self::Arc(arc) => arc.tangent_at(t),
        }
    }

    /// Distance from `p` to this finite segment.
    pub fn distance(&self, p: Pt) -> f64 {
        match *self {
            Self::Line { start, end } => {
                let d = end.sub(start);
                let len_sq = d.dot(d);
                if len_sq <= f64::EPSILON {
                    return p.distance(start);
                }
                let t = (p.sub(start).dot(d) / len_sq).clamp(0.0, 1.0);
                p.distance(start.add(d.scale(t)))
            }
            Self::Arc(arc) => arc.distance(p),
        }
    }

    /// Contribution of this directed segment to the closed-loop integral of `x dy`.
    pub fn signed_area_contribution(&self) -> f64 {
        match *self {
            Self::Line { start, end } => 0.5 * (start.x * end.y - end.x * start.y),
            Self::Arc(arc) => {
                // Integral of (x dy - y dx)/2 along the directed arc, written with an unwrapped
                // end angle so it stays correct across angle zero.
                let a0 = arc.start_angle;
                let a1 = arc.start_angle + arc.sweep;
                let r = arc.radius;
                0.5 * (r * r * (a1 - a0)
                    + r * arc.center.x * (a1.sin() - a0.sin())
                    + r * arc.center.y * (a0.cos() - a1.cos()))
            }
        }
    }

    /// Exact axis aligned extents contribution, adding points to `min`/`max`.
    fn extend_bounds(&self, min: &mut Pt, max: &mut Pt) {
        let mut add_point = |p: Pt| {
            if p.x < min.x {
                min.x = p.x;
            }
            if p.y < min.y {
                min.y = p.y;
            }
            if p.x > max.x {
                max.x = p.x;
            }
            if p.y > max.y {
                max.y = p.y;
            }
        };

        add_point(self.start());
        add_point(self.end());
        if let Self::Arc(arc) = *self {
            for k in 0..4 {
                let angle = k as f64 * PI * 0.5;
                if let Some(t) = arc.angle_param(angle, ARC_ANGLE_EPS) {
                    add_point(arc.point_at(t));
                }
            }
        }
    }
}

/// Build a finite arc from a bulge value, independently of the library.
///
/// Bulge is `tan(sweep/4)` and |sweep| <= PI. The center formula is the standard chord
/// construction: start + chord/2 + perp(chord) * (1 - b^2) / (4 b).
pub fn arc_from_bulge(start: Pt, end: Pt, bulge: f64) -> RefArc {
    let chord = end.sub(start);
    let chord_len = chord.length();
    let abs_bulge = bulge.abs();
    let radius = chord_len * (1.0 + bulge * bulge) / (4.0 * abs_bulge);
    let factor = (1.0 - bulge) * (1.0 + bulge) / (4.0 * bulge);
    let center = start.add(chord.scale(0.5)).add(chord.perp().scale(factor));
    let start_angle = (start.y - center.y).atan2(start.x - center.x);
    let sweep = 4.0 * bulge.atan();
    RefArc {
        start,
        end,
        center,
        radius,
        start_angle,
        sweep,
    }
}

/// A single closed (or open) directed boundary loop.
#[derive(Clone, Debug)]
pub struct RefLoop {
    pub segs: Vec<RefSeg>,
    pub closed: bool,
}

impl RefLoop {
    /// Build a loop from a polyline by reading only vertex positions and bulges.
    pub fn from_pline(pline: &Polyline<f64>) -> Self {
        let n = pline.vertex_count();
        let mut segs = Vec::new();
        if n >= 2 {
            let seg_count = if pline.is_closed() { n } else { n - 1 };
            for i in 0..seg_count {
                let v1 = pline.at(i);
                let v2 = pline.at((i + 1) % n);
                segs.push(seg_from_vertexes(v1, v2));
            }
        }
        Self {
            segs,
            closed: pline.is_closed(),
        }
    }

    /// Build a loop directly from vertices (positions and bulges), for transformed data.
    pub fn from_vertices(vertices: &[PlineVertex<f64>], closed: bool) -> Self {
        let n = vertices.len();
        let mut segs = Vec::new();
        if n >= 2 {
            let seg_count = if closed { n } else { n - 1 };
            for i in 0..seg_count {
                let v1 = vertices[i];
                let v2 = vertices[(i + 1) % n];
                segs.push(seg_from_vertexes(v1, v2));
            }
        }
        Self { segs, closed }
    }

    pub fn distance(&self, p: Pt) -> f64 {
        self.segs
            .iter()
            .map(|s| s.distance(p))
            .fold(f64::INFINITY, f64::min)
    }

    /// Signed area of the directed loop (positive for counter clockwise traversal).
    pub fn signed_area(&self) -> f64 {
        self.segs.iter().map(RefSeg::signed_area_contribution).sum()
    }

    /// Winding number of a closed loop about `p` by horizontal ray crossings.
    ///
    /// The loop is walked as one directed polyline of straight and circular pieces. Arcs are
    /// first split at their y extrema so every piece is y monotone and its ray intersection is
    /// unique. Pieces whose whole y span lies on one side of `p.y` are skipped, which gives the
    /// same half open convention to lines and arcs and counts a crossing at a shared vertex
    /// exactly once. Arcs crossing angle zero and tangent contacts therefore need no special
    /// cases. Crossings are counted only where the intersection is strictly to the right of `p`,
    /// and a horizontal piece touching the ray changes nothing.
    pub fn winding(&self, p: Pt) -> i32 {
        if !self.closed {
            return 0;
        }
        let mut winding = 0;
        let ray_eps = 1e-9 * (1.0 + p.y.abs());
        for seg in &self.segs {
            match *seg {
                RefSeg::Line { start, end } => {
                    let c0 = y_class(start.y, p.y, ray_eps);
                    let c1 = y_class(end.y, p.y, ray_eps);
                    // Half open rule: treat an on-ray endpoint as not-above, so a vertex lying on
                    // the ray is counted by exactly one of the two pieces meeting there, and a
                    // tangent touch nets to zero across its split pieces.
                    if (c0 > 0) != (c1 > 0) {
                        let x_int =
                            start.x + (p.y - start.y) * (end.x - start.x) / (end.y - start.y);
                        if x_int > p.x {
                            winding += if c1 > c0 { 1 } else { -1 };
                        }
                    }
                }
                RefSeg::Arc(arc) => {
                    for (sub, _t0, _t1) in monotone_arc_pieces(&arc) {
                        let c0 = y_class(sub.start.y, p.y, ray_eps);
                        let c1 = y_class(sub.end.y, p.y, ray_eps);
                        if (c0 > 0) != (c1 > 0) {
                            let x_cross = arc_x_at_y(&sub, p.y);
                            if x_cross > p.x {
                                winding += if c1 > c0 { 1 } else { -1 };
                            }
                        }
                    }
                }
            }
        }
        winding
    }

    /// Exact axis aligned bounds of the finite boundary geometry.
    pub fn bounds(&self) -> Option<(Pt, Pt)> {
        if self.segs.is_empty() {
            return None;
        }
        let first = self.segs[0].start();
        let mut min = first;
        let mut max = first;
        for seg in &self.segs {
            seg.extend_bounds(&mut min, &mut max);
        }
        Some((min, max))
    }

    /// Boundary samples at the given normalized parameters on every segment.
    pub fn boundary_samples(&self, params: &[f64]) -> Vec<(usize, f64, Pt, Pt)> {
        let mut result = Vec::new();
        for (idx, seg) in self.segs.iter().enumerate() {
            for &t in params {
                result.push((idx, t, seg.point_at(t), seg.tangent_at(t)));
            }
        }
        result
    }
}

fn seg_from_vertexes(v1: PlineVertex<f64>, v2: PlineVertex<f64>) -> RefSeg {
    let start = Pt::new(v1.x, v1.y);
    let end = Pt::new(v2.x, v2.y);
    if v1.bulge == 0.0 || start.distance(end) <= f64::EPSILON {
        RefSeg::Line { start, end }
    } else {
        RefSeg::Arc(arc_from_bulge(start, end, v1.bulge))
    }
}

/// Classify a y coordinate against the ray height with a tiny band: -1 below, 0 on the ray,
/// +1 above.
fn y_class(y: f64, py: f64, eps: f64) -> i32 {
    if (y - py).abs() <= eps {
        0
    } else if y > py {
        1
    } else {
        -1
    }
}

/// Split a finite arc at its y extrema into y monotone sub arcs.
///
/// Returns each sub arc with the normalized parameters it spans on the parent arc. Splitting this
/// way makes a horizontal ray meet each piece in at most one point (except at an extremum, where
/// the half open crossing rule discards the touch).
fn monotone_arc_pieces(arc: &RefArc) -> Vec<(RefArc, f64, f64)> {
    let mut cuts: Vec<f64> = vec![0.0, 1.0];
    for k in 0..4 {
        let angle = k as f64 * PI * 0.5;
        if let Some(t) = arc.angle_param(angle, ARC_ANGLE_EPS)
            && t > 1e-10
            && t < 1.0 - 1e-10
        {
            cuts.push(t);
        }
    }
    cuts.sort_by(f64::total_cmp);
    cuts.dedup_by(|a, b| (*a - *b).abs() <= 1e-12);
    let mut pieces = Vec::with_capacity(cuts.len() - 1);
    for w in cuts.windows(2) {
        let (t0, t1) = (w[0], w[1]);
        let sub = RefArc {
            start: arc.point_at(t0),
            end: arc.point_at(t1),
            center: arc.center,
            radius: arc.radius,
            start_angle: arc.start_angle + arc.sweep * t0,
            sweep: arc.sweep * (t1 - t0),
        };
        pieces.push((sub, t0, t1));
    }
    pieces
}

/// x coordinate where the y monotone arc `sub` crosses `y`, by solving on its support circle and
/// choosing the polarity that matches its direction of travel.
fn arc_x_at_y(sub: &RefArc, y: f64) -> f64 {
    let dy = (y - sub.center.y) / sub.radius;
    let dy = dy.clamp(-1.0, 1.0);
    let theta = dy.asin();
    let candidate0 = sub.center.x + sub.radius * theta.cos();
    let candidate1 = sub.center.x - sub.radius * theta.cos();
    // Pick the candidate whose polar angle lies on the sub arc's sweep.
    let pick = |x: f64| -> Option<f64> {
        let a = (y - sub.center.y).atan2(x - sub.center.x);
        sub.angle_param(a, ARC_ANGLE_EPS).map(|_| x)
    };
    pick(candidate0)
        .or_else(|| pick(candidate1))
        .unwrap_or(candidate0)
}

/// Reverse the traversal direction of a segment list.
pub fn reverse_segs(segs: &[RefSeg]) -> Vec<RefSeg> {
    segs.iter()
        .rev()
        .map(|s| match *s {
            RefSeg::Line { start, end } => RefSeg::Line {
                start: end,
                end: start,
            },
            RefSeg::Arc(arc) => RefSeg::Arc(RefArc {
                start: arc.end,
                end: arc.start,
                center: arc.center,
                radius: arc.radius,
                start_angle: arc.start_angle + arc.sweep,
                sweep: -arc.sweep,
            }),
        })
        .collect()
}

/// A shape region represented as directed boundary loops.
#[derive(Clone, Debug)]
pub struct RefShape {
    pub loops: Vec<RefLoop>,
}

impl RefShape {
    pub fn new(loops: Vec<RefLoop>) -> Self {
        Self { loops }
    }

    pub fn from_plines<I>(plines: I) -> Self
    where
        I: IntoIterator<Item = Polyline<f64>>,
    {
        Self::new(
            plines
                .into_iter()
                .map(|p| RefLoop::from_pline(&p))
                .collect(),
        )
    }

    /// Total winding number of the region (ccw loops add, cw holes subtract).
    pub fn winding(&self, p: Pt) -> i32 {
        self.loops.iter().map(|l| l.winding(p)).sum()
    }

    pub fn contains(&self, p: Pt) -> bool {
        self.winding(p) != 0
    }

    /// Minimum distance from `p` to any boundary segment.
    pub fn boundary_distance(&self, p: Pt) -> f64 {
        self.loops
            .iter()
            .map(|l| l.distance(p))
            .fold(f64::INFINITY, f64::min)
    }

    /// Signed distance: positive inside the region, negative outside.
    pub fn signed_distance(&self, p: Pt) -> f64 {
        let dist = self.boundary_distance(p);
        if self.contains(p) { dist } else { -dist }
    }

    /// Sum of independently computed loop signed areas.
    pub fn signed_area(&self) -> f64 {
        self.loops.iter().map(RefLoop::signed_area).sum()
    }

    pub fn bounds(&self) -> Option<(Pt, Pt)> {
        let mut min = Pt::new(f64::INFINITY, f64::INFINITY);
        let mut max = Pt::new(f64::NEG_INFINITY, f64::NEG_INFINITY);
        let mut any = false;
        for l in &self.loops {
            if let Some((lmin, lmax)) = l.bounds() {
                any = true;
                min.x = min.x.min(lmin.x);
                min.y = min.y.min(lmin.y);
                max.x = max.x.max(lmax.x);
                max.y = max.y.max(lmax.y);
            }
        }
        if any { Some((min, max)) } else { None }
    }
}

/// A similarity transform: optional reflection, then uniform rotation+scale, then translation.
#[derive(Clone, Copy, Debug)]
pub struct Sim {
    pub cos: f64,
    pub sin: f64,
    pub scale: f64,
    pub tx: f64,
    pub ty: f64,
    pub mirror: bool,
}

impl Sim {
    pub fn identity() -> Self {
        Self {
            cos: 1.0,
            sin: 0.0,
            scale: 1.0,
            tx: 0.0,
            ty: 0.0,
            mirror: false,
        }
    }

    pub fn rotation(angle: f64, tx: f64, ty: f64) -> Self {
        Self {
            cos: angle.cos(),
            sin: angle.sin(),
            scale: 1.0,
            tx,
            ty,
            mirror: false,
        }
    }

    pub fn translation(tx: f64, ty: f64) -> Self {
        Self {
            cos: 1.0,
            sin: 0.0,
            scale: 1.0,
            tx,
            ty,
            mirror: false,
        }
    }

    pub fn uniform_scale(scale: f64) -> Self {
        Self {
            cos: 1.0,
            sin: 0.0,
            scale,
            tx: 0.0,
            ty: 0.0,
            mirror: false,
        }
    }

    /// Reflection across the line y = x, achieved by reflect-x then rotate by +90 degrees.
    pub fn mirror_y_equals_x(tx: f64, ty: f64) -> Self {
        Self {
            cos: 0.0,
            sin: 1.0,
            scale: 1.0,
            tx,
            ty,
            mirror: true,
        }
    }

    pub fn label(&self) -> String {
        format!(
            "R({:.3},{:.3})*{:.3}+({:.3},{:.3}){}",
            self.cos,
            self.sin,
            self.scale,
            self.tx,
            self.ty,
            if self.mirror { " mirror" } else { "" }
        )
    }

    pub fn is_identity(&self) -> bool {
        !self.mirror
            && self.scale == 1.0
            && self.cos == 1.0
            && self.sin == 0.0
            && self.tx == 0.0
            && self.ty == 0.0
    }

    pub fn apply(&self, p: Pt) -> Pt {
        let q = if self.mirror { Pt::new(p.x, -p.y) } else { p };
        let rotated = Pt::new(
            self.cos * q.x - self.sin * q.y,
            self.sin * q.x + self.cos * q.y,
        );
        Pt::new(
            self.scale * rotated.x + self.tx,
            self.scale * rotated.y + self.ty,
        )
    }

    /// Apply the directional part (no translation) to a tangent vector.
    pub fn apply_dir(&self, v: Pt) -> Pt {
        let q = if self.mirror { Pt::new(v.x, -v.y) } else { v };
        let rotated = Pt::new(
            self.cos * q.x - self.sin * q.y,
            self.sin * q.x + self.cos * q.y,
        );
        rotated.scale(self.scale)
    }

    fn transform_seg(&self, seg: &RefSeg) -> RefSeg {
        match *seg {
            RefSeg::Line { start, end } => RefSeg::Line {
                start: self.apply(start),
                end: self.apply(end),
            },
            RefSeg::Arc(arc) => {
                let center = self.apply(arc.center);
                let start = self.apply(arc.start);
                let end = self.apply(arc.end);
                let radius = arc.radius * self.scale;
                let start_angle = (start.y - center.y).atan2(start.x - center.x);
                let sweep = if self.mirror { -arc.sweep } else { arc.sweep };
                RefSeg::Arc(RefArc {
                    start,
                    end,
                    center,
                    radius,
                    start_angle,
                    sweep,
                })
            }
        }
    }

    pub fn transform_shape(&self, shape: &RefShape) -> RefShape {
        RefShape::new(
            shape
                .loops
                .iter()
                .map(|l| {
                    let segs: Vec<RefSeg> = l.segs.iter().map(|s| self.transform_seg(s)).collect();
                    let segs = if self.mirror {
                        reverse_segs(&segs)
                    } else {
                        segs
                    };
                    RefLoop {
                        segs,
                        closed: l.closed,
                    }
                })
                .collect(),
        )
    }

    /// Transform polyline vertex data. Reflection negates bulges and, when `mirror` is set, the
    /// caller must reverse loops to restore outer ccw / hole cw orientation.
    pub fn transform_pline(&self, pline: &Polyline<f64>) -> Polyline<f64> {
        let mut vertices: Vec<PlineVertex<f64>> = pline
            .vertex_data
            .iter()
            .map(|v| {
                let p = self.apply(Pt::new(v.x, v.y));
                PlineVertex::new(p.x, p.y, if self.mirror { -v.bulge } else { v.bulge })
            })
            .collect();

        if self.mirror {
            vertices = reverse_closed_vertices(&vertices);
        }

        let mut result = Polyline::new();
        result.is_closed = pline.is_closed();
        result.vertex_data = vertices;
        result.userdata = pline.userdata.clone();
        result
    }

    pub fn transform_plines(&self, plines: &[Polyline<f64>]) -> Vec<Polyline<f64>> {
        plines.iter().map(|p| self.transform_pline(p)).collect()
    }
}

/// Reverse a closed loop's vertex order so traversal direction is inverted.
///
/// The segment from `w[j]` to `w[j+1]` is the original segment from `v[n-2-j]` to `v[n-1-j]`
/// traversed backwards, so its bulge is the negated original bulge.
pub fn reverse_closed_vertices(vertices: &[PlineVertex<f64>]) -> Vec<PlineVertex<f64>> {
    let n = vertices.len();
    let mut result = Vec::with_capacity(n);
    for j in 0..n {
        let pos_idx = (n - 1 - j) % n;
        let bulge_idx = (n + 2 - j) % n;
        let v = vertices[pos_idx];
        result.push(PlineVertex::new(v.x, v.y, -vertices[bulge_idx].bulge));
    }
    result
}

/// Bit level snapshot of polyline input loop data for the unchanged-input check.
pub fn pline_bits(pline: &Polyline<f64>) -> Vec<u64> {
    let mut bits = Vec::with_capacity(pline.vertex_data.len() * 3 + 2);
    for v in &pline.vertex_data {
        bits.push(v.x.to_bits());
        bits.push(v.y.to_bits());
        bits.push(v.bulge.to_bits());
    }
    bits.push(u64::from(pline.is_closed));
    bits.extend(pline.userdata.iter().copied());
    bits
}

/// Bit level snapshot of all input loops.
pub fn plines_bits(plines: &[Polyline<f64>]) -> Vec<Vec<u64>> {
    plines.iter().map(pline_bits).collect()
}
