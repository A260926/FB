//! Closed shape fixtures used by the reference regression.
//!
//! Every fixture is built from directed line segments and circular arcs expressed as polyline
//! vertexes (position + bulge). Outer loops are counter clockwise and hole loops clockwise.
//! Fixtures carry hand-derived topology, base areas and boundary calibration points so the
//! independent reference module itself is checked against analytic values.

#![allow(dead_code)]

use cavalier_contours::polyline::{PlineVertex, Polyline};

use super::geometry::{Pt, reverse_closed_vertices};

/// Bulge for a 90 degree arc sweep.
pub const Q: f64 = 0.414_213_562_373_095_03;

/// One offset case of a fixture.
#[derive(Clone, Copy, Debug)]
pub struct Case {
    pub offset: f64,
    /// Expected filled components (outer loops) in the offset result.
    pub components: usize,
    /// Expected holes (clockwise loops) in the offset result.
    pub holes: usize,
}

/// A closed fixture scenario.
#[derive(Clone, Debug)]
pub struct Fixture {
    pub name: &'static str,
    pub plines: Vec<Polyline<f64>>,
    pub cases: Vec<Case>,
    pub area_model: Option<AreaModel>,
    /// Points in base coordinates and their hand-computed distance to the source boundary.
    pub calibration: Vec<(Pt, f64)>,
    /// Hand-computed signed area of the source region.
    pub base_area: Option<f64>,
}

/// Analytic area models for offset regions, written for the specific fixture constructions.
#[derive(Clone, Copy, Debug)]
pub enum AreaModel {
    /// Concentric circular ring.
    Ring { outer_radius: f64, hole_radius: f64 },
    /// Capsule: two semicircular caps joined by straight parallel sides.
    Capsule { radius: f64, straight_len: f64 },
    /// Two equal discs separated by `center_dist`, merging once their radii overlap.
    TwoDiscs { radius: f64, center_dist: f64 },
}

impl AreaModel {
    /// Hand-computed area of the region after offset `d` (positive shrinks the region).
    pub fn expected_area(&self, d: f64) -> f64 {
        match *self {
            Self::Ring {
                outer_radius,
                hole_radius,
            } => {
                let outer = outer_radius - d;
                let hole = hole_radius + d;
                if outer <= 0.0 {
                    0.0
                } else if hole <= 0.0 {
                    // The enlarged hole boundary contracted through zero radius, so the hole is
                    // gone and the region is the full eroded disc.
                    std::f64::consts::PI * outer * outer
                } else if hole >= outer {
                    0.0
                } else {
                    std::f64::consts::PI * (outer * outer - hole * hole)
                }
            }
            Self::Capsule {
                radius,
                straight_len,
            } => {
                let half_width = radius - d;
                if half_width <= 0.0 {
                    0.0
                } else {
                    2.0 * straight_len * half_width + std::f64::consts::PI * half_width * half_width
                }
            }
            Self::TwoDiscs {
                radius,
                center_dist,
            } => {
                let rho = radius - d;
                if rho <= 0.0 {
                    return 0.0;
                }
                let half_center = center_dist * 0.5;
                if half_center >= rho {
                    2.0 * std::f64::consts::PI * rho * rho
                } else {
                    let lens = 2.0 * rho * rho * (half_center / rho).acos()
                        - 2.0 * half_center * (rho * rho - half_center * half_center).sqrt();
                    2.0 * std::f64::consts::PI * rho * rho - lens
                }
            }
        }
    }

    /// Hand-computed exact bounds of the offset region, or `None` when empty.
    pub fn expected_bounds(&self, d: f64) -> Option<(Pt, Pt)> {
        match *self {
            Self::Ring {
                outer_radius,
                hole_radius,
            } => {
                let outer = outer_radius - d;
                let hole = hole_radius + d;
                if outer <= 0.0 || (hole > 0.0 && hole >= outer) {
                    None
                } else {
                    Some((Pt::new(-outer, -outer), Pt::new(outer, outer)))
                }
            }
            Self::Capsule {
                radius,
                straight_len,
            } => {
                let half_width = radius - d;
                if half_width <= 0.0 {
                    None
                } else {
                    let half_len = straight_len * 0.5 + half_width;
                    Some((
                        Pt::new(-half_len, -half_width),
                        Pt::new(half_len, half_width),
                    ))
                }
            }
            Self::TwoDiscs {
                radius,
                center_dist,
            } => {
                let rho = radius - d;
                if rho <= 0.0 {
                    None
                } else {
                    let half_center = center_dist * 0.5;
                    Some((
                        Pt::new(-half_center - rho, -rho),
                        Pt::new(half_center + rho, rho),
                    ))
                }
            }
        }
    }
}

fn pline(closed: bool, vertices: &[(f64, f64, f64)]) -> Polyline<f64> {
    let mut p = Polyline::new();
    p.is_closed = closed;
    p.vertex_data = vertices
        .iter()
        .map(|&(x, y, bulge)| PlineVertex::new(x, y, bulge))
        .collect();
    p
}

fn pline_vertices(vertices: &[PlineVertex<f64>]) -> Polyline<f64> {
    let mut p = Polyline::new();
    p.is_closed = true;
    p.vertex_data = vertices.to_vec();
    p
}

/// Build a counter clockwise rounded rectangle with straight sides and quarter arc corners.
/// When `ccw` is false the vertex order is reversed to produce a clockwise hole loop.
fn rounded_rect(center: Pt, half_w: f64, half_h: f64, corner: f64, ccw: bool) -> Polyline<f64> {
    let cx = center.x;
    let cy = center.y;
    let sx = half_w - corner;
    let sy = half_h - corner;
    let mut vertices: Vec<PlineVertex<f64>> = [
        (cx - sx, cy - half_h, 0.0),
        (cx + sx, cy - half_h, Q),
        (cx + half_w, cy - sy, 0.0),
        (cx + half_w, cy + sy, Q),
        (cx + sx, cy + half_h, 0.0),
        (cx - sx, cy + half_h, Q),
        (cx - half_w, cy + sy, 0.0),
        (cx - half_w, cy - sy, Q),
    ]
    .iter()
    .map(|&(x, y, bulge)| PlineVertex::new(x, y, bulge))
    .collect();
    if !ccw {
        vertices = reverse_closed_vertices(&vertices);
    }
    pline_vertices(&vertices)
}

/// Circle polyline of radius `radius` centred at `center` (two half circle arcs).
fn circle(center: Pt, radius: f64, ccw: bool) -> Polyline<f64> {
    let sign = if ccw { 1.0 } else { -1.0 };
    pline(
        true,
        &[
            (center.x + radius, center.y, sign),
            (center.x - radius, center.y, sign),
        ],
    )
}

/// Analytic area of a rounded rectangle.
fn rounded_rect_area(half_w: f64, half_h: f64, corner: f64) -> f64 {
    4.0 * half_w * half_h - (4.0 - std::f64::consts::PI) * corner * corner
}

/// Convert a positive radian sweep in `(0, PI]` into a bulge.
fn sweep_bulge(sweep: f64) -> f64 {
    (sweep * 0.25).tan()
}

/// Ring fixture: outer radius 4, hole radius 1.5.
pub fn ring() -> Fixture {
    const R: f64 = 4.0;
    const RH: f64 = 1.5;
    let mut calibration = Vec::new();
    for k in 0..12 {
        let angle = 0.37 + k as f64 * 0.523;
        let dir = Pt::new(angle.cos(), angle.sin());
        // Material mid radius: equal distance to both circular boundaries.
        calibration.push((dir.scale((R + RH) * 0.5), (R - RH) * 0.5));
        // Outside and inside points with known radial distance.
        calibration.push((dir.scale(R + 0.75), 0.75));
        calibration.push((dir.scale(RH - 0.5), 0.5));
    }
    Fixture {
        name: "ring",
        plines: vec![
            circle(Pt::new(0.0, 0.0), R, true),
            circle(Pt::new(0.0, 0.0), RH, false),
        ],
        cases: vec![
            // Hole grows, material band narrows then closes exactly between the two.
            Case {
                offset: 1.20,
                components: 1,
                holes: 1,
            },
            Case {
                offset: 1.30,
                components: 0,
                holes: 0,
            },
            Case {
                offset: 1.00,
                components: 1,
                holes: 1,
            },
            // Dilations keep one hole.
            Case {
                offset: -1.00,
                components: 1,
                holes: 1,
            },
            // Expanding outer boundary outruns the shrinking hole: hole disappears.
            Case {
                offset: -1.80,
                components: 1,
                holes: 0,
            },
        ],
        area_model: Some(AreaModel::Ring {
            outer_radius: R,
            hole_radius: RH,
        }),
        calibration,
        base_area: Some(std::f64::consts::PI * (R * R - RH * RH)),
    }
}

/// Capsule fixture: straight length 3 between cap centers, cap radius 1.5.
pub fn capsule() -> Fixture {
    const R: f64 = 1.5;
    const L: f64 = 3.0;
    let hl = L * 0.5;
    let hr = R;
    let plines = vec![pline(
        true,
        &[
            (-hl, -hr, 0.0), // bottom straight line start
            (hl, -hr, 1.0),  // right cap (half circle)
            (hl, hr, 0.0),   // top straight line start
            (-hl, hr, 1.0),  // left cap (half circle)
        ],
    )];
    let mut calibration = Vec::new();
    for k in 0..8 {
        let x = -hl + (k as f64 + 0.5) * L / 8.0;
        // Points above the straight section: nearest boundary is the straight side.
        calibration.push((Pt::new(x, hr + 0.6), 0.6));
        // Centerline points: nearest boundary is the straight side at distance R.
        calibration.push((Pt::new(x, 0.0), R));
    }
    for k in 0..6 {
        let angle = -std::f64::consts::FRAC_PI_2 + (k as f64 + 0.5) * std::f64::consts::PI / 6.0;
        let dir = Pt::new(angle.cos(), angle.sin());
        calibration.push((Pt::new(hl, 0.0).add(dir.scale(R)), 0.0));
        calibration.push((Pt::new(hl, 0.0).add(dir.scale(R + 0.4)), 0.4));
    }
    Fixture {
        name: "capsule",
        plines,
        cases: vec![
            Case {
                offset: 0.60,
                components: 1,
                holes: 0,
            },
            Case {
                offset: 1.40,
                components: 1,
                holes: 0,
            },
            // Inward past the cap radius collapses the whole region.
            Case {
                offset: 1.60,
                components: 0,
                holes: 0,
            },
            Case {
                offset: -0.90,
                components: 1,
                holes: 0,
            },
        ],
        area_model: Some(AreaModel::Capsule {
            radius: R,
            straight_len: L,
        }),
        calibration,
        base_area: Some(2.0 * L * R + std::f64::consts::PI * R * R),
    }
}

/// Dumbbell: two radius 1.5 lobes centred at `±2.5` joined by a straight neck of half width 0.4.
///
/// The neck sides are the horizontal lines `y = ±0.4` between the chord points where they meet
/// each circle. Region area is not modelled analytically; topology is hand derived:
/// inward offset below 0.4 keeps one component, above 0.4 splits the lobes, and past 1.5 the
/// region vanishes.
pub fn dumbbell() -> Fixture {
    const R: f64 = 1.5;
    const C: f64 = 2.5;
    const WN: f64 = 0.4;
    let dx = (R * R - WN * WN).sqrt();
    // Chord points of the neck on each lobe: `x_center -+ dx`, with |y| = WN.
    let lx = -C;
    let rx = C;
    // Sweep of the short cap arc on the side away from the neck, and of the major lobe arcs.
    let cap = sweep_bulge(2.0 * (WN / R).asin());
    let major = sweep_bulge(std::f64::consts::PI - 2.0 * (WN / R).asin());

    // Counter clockwise traversal: start at the left lobe's upper neck chord point.
    let vertices = vec![
        PlineVertex::new(lx + dx, WN, major), // upper major arc to left tip
        PlineVertex::new(lx - dx, WN, cap),   // left tip arc down
        PlineVertex::new(lx - dx, -WN, major), // lower major arc to lower neck point
        PlineVertex::new(lx + dx, -WN, 0.0),  // neck bottom line rightwards
        PlineVertex::new(rx - dx, -WN, major), // right lobe lower major arc
        PlineVertex::new(rx + dx, -WN, cap),  // right tip arc up
        PlineVertex::new(rx + dx, WN, major), // right lobe upper major arc
        PlineVertex::new(rx - dx, WN, 0.0),   // neck top line leftwards, closes
    ];

    let calibration = vec![
        (Pt::new(0.0, 0.0), WN),
        (Pt::new(0.0, WN), 0.0),
        (Pt::new(0.0, 0.9), 0.5),
        (Pt::new(lx, 0.0), R),
        (Pt::new(rx, 0.0), R),
        (Pt::new(lx - R, 0.0), 0.0),
        (Pt::new(rx + R, 0.0), 0.0),
    ];
    Fixture {
        name: "dumbbell",
        plines: vec![pline_vertices(&vertices)],
        cases: vec![
            // Neck half width 0.4: erosion below it keeps the two lobes joined.
            Case {
                offset: 0.25,
                components: 1,
                holes: 0,
            },
            // Erosion past the neck half width splits the region into two components.
            Case {
                offset: 0.45,
                components: 2,
                holes: 0,
            },
            Case {
                offset: -0.60,
                components: 1,
                holes: 0,
            },
            Case {
                offset: 1.60,
                components: 0,
                holes: 0,
            },
        ],
        area_model: None,
        calibration,
        base_area: None,
    }
}

/// Two disjoint discs of radius 1.0 with centers 4.0 apart.
pub fn two_discs() -> Fixture {
    const R: f64 = 1.0;
    const C: f64 = 2.0;
    let mut calibration = Vec::new();
    for k in 0..6 {
        let angle = 0.4 + k as f64 * 1.0;
        let dir = Pt::new(angle.cos(), angle.sin());
        calibration.push((Pt::new(-C, 0.0).add(dir.scale(R + 0.5)), 0.5));
        calibration.push((Pt::new(C, 0.0), R));
        // Midpoint between the discs: nearest boundary is the facing side of either disc,
        // at distance C - R.
        calibration.push((Pt::new(0.0, 0.0), C - R));
    }
    Fixture {
        name: "two_discs",
        plines: vec![
            circle(Pt::new(-C, 0.0), R, true),
            circle(Pt::new(C, 0.0), R, true),
        ],
        cases: vec![
            // Outward dilation: components stay separate until the discs overlap.
            Case {
                offset: -0.70,
                components: 2,
                holes: 0,
            },
            Case {
                offset: -1.30,
                components: 1,
                holes: 0,
            },
            Case {
                offset: 0.50,
                components: 2,
                holes: 0,
            },
        ],
        area_model: Some(AreaModel::TwoDiscs {
            radius: R,
            center_dist: 2.0 * C,
        }),
        calibration,
        base_area: Some(2.0 * std::f64::consts::PI * R * R),
    }
}

/// Outer rounded rectangle with a rounded-corner hole.
///
/// Outer: half extents 5 and 3, corner radius 1.5, centered at the origin.
/// Hole: half extents 2 and 1, corner radius 0.6, centered at (0.8, -0.3), clockwise.
pub fn rounded_hole() -> Fixture {
    let outer = rounded_rect(Pt::new(0.0, 0.0), 5.0, 3.0, 1.5, true);
    let hole = rounded_rect(Pt::new(0.8, -0.3), 2.0, 1.0, 0.6, false);
    let calibration = vec![
        (Pt::new(0.0, -3.0), 0.0),
        (Pt::new(0.0, -2.5), 0.5),
        (Pt::new(0.0, -1.45), 0.15),
        (Pt::new(3.4, 0.0), 0.6),
        (Pt::new(5.0, 0.0), 0.0),
        (Pt::new(-5.0, 0.0), 0.0),
        (Pt::new(0.0, 3.0), 0.0),
    ];
    Fixture {
        name: "rounded_hole",
        plines: vec![outer, hole],
        cases: vec![
            // Hole expands but stays inside, region has one hole.
            Case {
                offset: 0.60,
                components: 1,
                holes: 1,
            },
            // Hole breaks through the bottom outer boundary: material becomes an open arch.
            Case {
                offset: 0.90,
                components: 1,
                holes: 0,
            },
            // Hole expansion consumes the whole eroded outer region.
            Case {
                offset: 2.00,
                components: 0,
                holes: 0,
            },
            // Hole shrinks but survives.
            Case {
                offset: -0.50,
                components: 1,
                holes: 1,
            },
            // Hole disappears entirely.
            Case {
                offset: -1.20,
                components: 1,
                holes: 0,
            },
        ],
        area_model: None,
        calibration,
        base_area: Some(rounded_rect_area(5.0, 3.0, 1.5) - rounded_rect_area(2.0, 1.0, 0.6)),
    }
}

/// All fixtures in a fixed order.
pub fn all() -> Vec<Fixture> {
    vec![ring(), capsule(), rounded_hole(), dumbbell(), two_discs()]
}
