//! Harness turning a fixture case into an offset call and a fully independent verdict.
//!
//! Expected values come only from [`super::geometry`] (distances, winding, area) and the
//! analytic fixture models. The library is used solely to produce the output polylines through
//! `Shape::from_plines` and `Shape::parallel_offset`.

#![allow(dead_code)]

use cavalier_contours::polyline::Polyline;
use cavalier_contours::shape_algorithms::{Shape, ShapeOffsetOptions};

use super::fixtures::{Case, Fixture};
use super::geometry::{Pt, RefLoop, RefShape, Sim};

/// Marker prefixed to every geometric assertion message so the mutant script can distinguish a
/// genuine numeric rejection from a crash, compile error or timeout.
pub const ASSERT_MARKER: &str = "OFFSET_REFERENCE_ASSERT";

/// Maximum numeric protection band, in original/base length units. Points whose signed distance
/// to the target boundary is closer than this are reported as undecided and never asserted.
pub const GUARD_BASE: f64 = 5e-4;

/// Tolerance for a sampled output boundary point to lie on the analytic target boundary. This is
/// an assertion tolerance, not a skip band; the skip band is [`GUARD_BASE`].
pub const BOUNDARY_TOL_BASE: f64 = 1e-3;

/// Relative tolerance for analytic area agreement.
pub const AREA_REL_TOL: f64 = 1e-6;

/// Number of irregular grid probes over the expanded fixture bounds.
pub const GRID_PROBES: usize = 2400;

/// Nominal probe offset along the output boundary normal, in base units.
pub const NORMAL_PROBE_BASE: f64 = 0.05;

/// Per-case verdict plus the values printed for every scenario.
#[derive(Clone, Debug)]
pub struct Report {
    pub fixture: &'static str,
    pub transform: String,
    pub offset: f64,
    pub components: usize,
    pub holes: usize,
    pub valid_probes: usize,
    pub skipped_probes: usize,
    pub boundary_probes: usize,
    pub max_deviation: f64,
    pub area: Option<(f64, f64)>,
    pub failures: Vec<String>,
}

impl Report {
    pub fn skip_ratio(&self) -> f64 {
        let total = self.valid_probes + self.skipped_probes;
        if total == 0 {
            0.0
        } else {
            self.skipped_probes as f64 / total as f64
        }
    }

    pub fn print(&self) {
        println!(
            "CASE fixture={} transform={} offset={:.6} components={} holes={} \
             valid_probes={} skipped_guard={} skip_ratio={:.4} normal_probes={} max_deviation={:.3e}",
            self.fixture,
            self.transform,
            self.offset,
            self.components,
            self.holes,
            self.valid_probes,
            self.skipped_probes,
            self.skip_ratio(),
            self.boundary_probes,
            self.max_deviation,
        );
        if let Some((actual, expected)) = self.area {
            println!(
                "  AREA fixture={} offset={:.6} independent={:.6} analytic={:.6} delta={:.3e}",
                self.fixture,
                self.offset,
                actual,
                expected,
                actual - expected
            );
        }
        for failure in &self.failures {
            println!("  FAIL {}", failure);
        }
    }

    pub fn assert_ok(&self) {
        assert!(
            self.failures.is_empty(),
            "{ASSERT_MARKER} {} transform={} offset={} components={} holes={} \
             valid_probes={} skipped_guard={} max_deviation={:.3e} first_failure: {}",
            self.fixture,
            self.transform,
            self.offset,
            self.components,
            self.holes,
            self.valid_probes,
            self.skipped_probes,
            self.max_deviation,
            self.failures.first().map(String::as_str).unwrap_or("none"),
        );
    }
}

/// A built run: transformed input, the `Shape` and its offset result.
pub struct OffsetRun {
    pub input: Vec<Polyline<f64>>,
    pub shape: Shape<f64>,
    pub result: Shape<f64>,
    pub input_bits: Vec<Vec<u64>>,
}

/// Scale the three `ShapeOffsetOptions` tolerances by the transform length scale.
pub fn scaled_options(scale: f64) -> ShapeOffsetOptions<f64> {
    let mut opts = ShapeOffsetOptions::new();
    opts.pos_equal_eps *= scale;
    opts.offset_dist_eps *= scale;
    opts.slice_join_eps *= scale;
    opts
}

/// Build transformed input polylines. Mirroring also restores loop orientation (outer ccw and
/// hole cw) by reversing vertex order.
pub fn build_input(fx: &Fixture, sim: &Sim) -> Vec<Polyline<f64>> {
    sim.transform_plines(&fx.plines)
}

/// Build a shape from input and perform one offset.
pub fn run_offset(fx: &Fixture, sim: &Sim, offset_base: f64) -> OffsetRun {
    let input = build_input(fx, sim);
    let input_bits = super::geometry::plines_bits(&input);
    let shape = Shape::from_plines(input.clone());
    let result = shape.parallel_offset(offset_base * sim.scale, &scaled_options(sim.scale));
    OffsetRun {
        input,
        shape,
        result,
        input_bits,
    }
}

/// Independent reference shape of the transformed input.
pub fn source_reference(run: &OffsetRun) -> RefShape {
    RefShape::new(run.input.iter().map(RefLoop::from_pline).collect())
}

/// Independent reference shape of the output loops, using their stored direction.
pub fn output_reference(result: &Shape<f64>) -> RefShape {
    let mut loops = Vec::new();
    for p in &result.ccw_plines {
        loops.push(RefLoop::from_pline(&p.polyline));
    }
    for p in &result.cw_plines {
        loops.push(RefLoop::from_pline(&p.polyline));
    }
    RefShape::new(loops)
}

/// Deterministic irregular probe grid in base coordinates over the fixture bounds expanded by
/// `expand`. Halton radical inverse in bases 2 and 3 gives a fixed, non-lattice distribution.
pub fn probe_grid(source: &RefShape, expand: f64) -> Vec<Pt> {
    let (min, max) = source.bounds().expect("fixture has boundary geometry");
    let width = max.x - min.x + 2.0 * expand;
    let height = max.y - min.y + 2.0 * expand;
    let lo_x = min.x - expand;
    let lo_y = min.y - expand;
    let mut probes = Vec::with_capacity(GRID_PROBES);
    for i in 1..=GRID_PROBES as u32 {
        let u = radical_inverse(i, 2);
        let v = radical_inverse(i, 3);
        probes.push(Pt::new(lo_x + u * width, lo_y + v * height));
    }
    probes
}

fn radical_inverse(mut index: u32, base: u32) -> f64 {
    let mut result = 0.0;
    let mut factor = 1.0 / base as f64;
    while index > 0 {
        result += (index % base) as f64 * factor;
        index /= base;
        factor /= base as f64;
    }
    result
}

/// Sample parameters used for boundary point checks and normal probes.
const BOUNDARY_PARAMS: [f64; 3] = [0.25, 0.5, 0.75];

/// Evaluate one case against the independent reference oracle.
pub fn evaluate(fx: &Fixture, case: &Case, sim: &Sim, run: &OffsetRun) -> Report {
    let source = source_reference(run);
    let output = output_reference(&run.result);
    let scale = sim.scale;
    let offset = case.offset * scale;
    let guard = GUARD_BASE * scale;
    let boundary_tol = BOUNDARY_TOL_BASE * scale;
    let normal_h = NORMAL_PROBE_BASE * scale;

    let mut failures: Vec<String> = Vec::new();
    let mut max_deviation: f64 = 0.0;

    // Independent loop classification and closure.
    let mut components = 0usize;
    let mut holes = 0usize;
    let mut loop_areas: Vec<f64> = Vec::new();
    for (idx, lp) in output.loops.iter().enumerate() {
        if !lp.closed {
            failures.push(format!("output loop {idx} is not closed"));
        }
        if lp.segs.is_empty() {
            failures.push(format!("output loop {idx} has no segments"));
            loop_areas.push(0.0);
            continue;
        }
        for (seg_idx, seg) in lp.segs.iter().enumerate() {
            if seg.start().distance(seg.end()) <= f64::EPSILON * scale {
                failures.push(format!(
                    "output loop {idx} segment {seg_idx} is zero length ({},{})-({},{})",
                    seg.start().x,
                    seg.start().y,
                    seg.end().x,
                    seg.end().y
                ));
            }
        }
        // Independent closure: consecutive pieces must share endpoints and the loop must return
        // to its starting point without comparing vertex order against any expectation.
        let gap_tol = 1e-6 * scale;
        for seg_idx in 0..lp.segs.len() {
            let end = lp.segs[seg_idx].end();
            let next_start = lp.segs[(seg_idx + 1) % lp.segs.len()].start();
            let gap = end.distance(next_start);
            if gap > gap_tol {
                failures.push(format!(
                    "output loop {idx} is not closed between segment {seg_idx} end \
                     ({:.9},{:.9}) and next start ({:.9},{:.9}) gap={gap:.3e}",
                    end.x, end.y, next_start.x, next_start.y
                ));
            }
        }
        let area = lp.signed_area();
        if area.abs() <= 1e-12 * scale * scale {
            failures.push(format!(
                "output loop {idx} has degenerate independent area {area:.6e}"
            ));
        } else if area > 0.0 {
            components += 1;
        } else {
            holes += 1;
        }
        loop_areas.push(area);
    }

    // Library class must agree with the independent orientation.
    for (idx, p) in run.result.ccw_plines.iter().enumerate() {
        let area = RefLoop::from_pline(&p.polyline).signed_area();
        if area <= 0.0 {
            failures.push(format!(
                "ccw_plines[{idx}] has independent signed area {area:.6e} (expected > 0)"
            ));
        }
    }
    for (idx, p) in run.result.cw_plines.iter().enumerate() {
        let area = RefLoop::from_pline(&p.polyline).signed_area();
        if area >= 0.0 {
            failures.push(format!(
                "cw_plines[{idx}] has independent signed area {area:.6e} (expected < 0)"
            ));
        }
    }

    // Every hole must be contained in material (positive winding from outer loops only).
    let outer_loops: Vec<&RefLoop> = output
        .loops
        .iter()
        .zip(loop_areas.iter())
        .filter_map(|(l, &a)| if a > 0.0 { Some(l) } else { None })
        .collect();
    for (idx, (lp, &area)) in output.loops.iter().zip(loop_areas.iter()).enumerate() {
        if area < 0.0 && !outer_loops.is_empty() {
            let sample = sample_segment(lp, 0, 0.5);
            let mother = outer_loops.iter().map(|l| l.winding(sample)).sum::<i32>();
            if mother == 0 {
                failures.push(format!(
                    "hole loop {idx} sample ({:.3},{:.3}) is not inside any outer loop",
                    sample.x, sample.y
                ));
            }
        }
    }

    // Irregular grid membership vs independent signed distance threshold. `source` is already in
    // transformed space, so the probes are used directly.
    let grid = probe_grid(&source, 2.6);
    let mut valid_probes = 0usize;
    let mut skipped = 0usize;
    for &p in &grid {
        let sd = source.signed_distance(p);
        let margin = sd - offset;
        if margin.abs() <= guard {
            skipped += 1;
            continue;
        }
        valid_probes += 1;
        let expected = margin > 0.0;
        let actual = output.winding(p) != 0;
        if expected != actual {
            let depth = margin.abs();
            max_deviation = max_deviation.max(depth);
            failures.push(format!(
                "membership mismatch at probe ({:.6},{:.6}) sd={sd:.6} offset={offset:.6} \
                 margin={margin:.6} expected_inside={expected} actual_inside={actual}",
                p.x, p.y
            ));
        }
    }

    // Boundary points must lie on the analytic target boundary, plus normal probes on both sides.
    let mut boundary_probes = 0usize;
    for (loop_idx, lp) in output.loops.iter().enumerate() {
        for (seg_idx, seg) in lp.segs.iter().enumerate() {
            for &t in &BOUNDARY_PARAMS {
                let p = seg.point_at(t);
                let sd = source.signed_distance(p);
                let deviation = (sd - offset).abs();
                max_deviation = max_deviation.max(deviation);
                if deviation > boundary_tol {
                    failures.push(format!(
                        "output boundary point loop={loop_idx} seg={seg_idx} t={t:.3} \
                         point=({:.6},{:.6}) sd={sd:.6} target_offset={offset:.6} \
                         deviation={deviation:.6}",
                        p.x, p.y
                    ));
                }
                let tangent = seg.tangent_at(t);
                let normal = tangent.perp();
                if normal.length() > 0.0 {
                    for side in [-1.0, 1.0] {
                        let probe = p.add(normal.scale(side * normal_h));
                        let psd = source.signed_distance(probe);
                        let margin = psd - offset;
                        if margin.abs() <= guard {
                            skipped += 1;
                            continue;
                        }
                        boundary_probes += 1;
                        let expected = margin > 0.0;
                        let actual = output.winding(probe) != 0;
                        if expected != actual {
                            let depth = margin.abs();
                            max_deviation = max_deviation.max(depth);
                            failures.push(format!(
                                "normal probe mismatch loop={loop_idx} seg={seg_idx} t={t:.3} \
                                 side={side:.0} point=({:.6},{:.6}) sd={psd:.6} \
                                 offset={offset:.6} margin={margin:.6} \
                                 expected_inside={expected} actual_inside={actual}",
                                probe.x, probe.y
                            ));
                        }
                    }
                }
            }
        }
    }

    // Independent area vs analytic model.
    let independent_area = output.signed_area();
    let mut area_pair = None;
    if let Some(model) = fx.area_model {
        let expected = model.expected_area(case.offset) * scale * scale;
        area_pair = Some((independent_area, expected));
        let tol = AREA_REL_TOL * expected.abs().max(1.0) * scale * scale + 1e-8 * scale * scale;
        let delta = (independent_area - expected).abs();
        max_deviation = max_deviation.max(delta);
        if delta > tol {
            failures.push(format!(
                "area mismatch independent={independent_area:.9} analytic={expected:.9} \
                 delta={delta:.3e} tol={tol:.3e}"
            ));
        }
    }

    // Exact analytic bounds are only axis aligned for the identity transform.
    if sim.is_identity()
        && let Some(model) = fx.area_model
    {
        let expected_bounds = model.expected_bounds(case.offset);
        let actual_bounds = output.bounds();
        match (expected_bounds, actual_bounds) {
            (None, Some((min, max))) => failures.push(format!(
                "expected empty bounds but output bounds are ({:.6},{:.6})-({:.6},{:.6})",
                min.x, min.y, max.x, max.y
            )),
            (Some((emin, emax)), None) => failures.push(format!(
                "expected bounds ({:.6},{:.6})-({:.6},{:.6}) but output is empty",
                emin.x, emin.y, emax.x, emax.y
            )),
            (Some((emin, emax)), Some((amin, amax))) => {
                let dmin = (amin.x - emin.x)
                    .abs()
                    .max((amin.y - emin.y).abs())
                    .max((amax.x - emax.x).abs())
                    .max((amax.y - emax.y).abs());
                max_deviation = max_deviation.max(dmin);
                if dmin > 1e-5 * scale {
                    failures.push(format!(
                        "bounds mismatch expected ({:.6},{:.6})-({:.6},{:.6}) \
                         actual ({:.6},{:.6})-({:.6},{:.6}) deviation={dmin:.6}",
                        emin.x, emin.y, emax.x, emax.y, amin.x, amin.y, amax.x, amax.y
                    ));
                }
            }
            (None, None) => {}
        }
    }

    // An empty region must have no library output either.
    if components == 0 && holes == 0 && (case.components != 0 || case.holes != 0) {
        failures.push(format!(
            "output has no loops but fixture case expects components={} holes={}",
            case.components, case.holes
        ));
    }
    if components != case.components || holes != case.holes {
        failures.push(format!(
            "topology mismatch independent components={components} holes={holes} \
             expected components={} holes={}",
            case.components, case.holes
        ));
    }

    Report {
        fixture: fx.name,
        transform: sim.label(),
        offset,
        components,
        holes,
        valid_probes,
        skipped_probes: skipped,
        boundary_probes,
        max_deviation,
        area: area_pair,
        failures,
    }
}

fn sample_segment(lp: &RefLoop, seg_idx: usize, t: f64) -> Pt {
    lp.segs[seg_idx % lp.segs.len()].point_at(t)
}

/// Compare two offset results independently of vertex order: topology counts, independent signed
/// area and region membership on the base probe grid.
pub fn semantic_diff(
    fx: &Fixture,
    sim: &Sim,
    a: &Shape<f64>,
    b: &Shape<f64>,
    label: &str,
) -> Vec<String> {
    let source = {
        let input = build_input(fx, sim);
        RefShape::new(input.iter().map(RefLoop::from_pline).collect())
    };
    let ref_a = output_reference(a);
    let ref_b = output_reference(b);
    let mut failures = Vec::new();

    let count_sign = |shape: &RefShape| {
        let mut components = 0usize;
        let mut holes = 0usize;
        for l in &shape.loops {
            let area = l.signed_area();
            if area > 0.0 {
                components += 1;
            } else {
                holes += 1;
            }
        }
        (components, holes)
    };
    let (ca, ha) = count_sign(&ref_a);
    let (cb, hb) = count_sign(&ref_b);
    if (ca, ha) != (cb, hb) {
        failures.push(format!(
            "{label}: topology differs fresh components={ca} holes={ha} \
             reused components={cb} holes={hb}"
        ));
    }

    let area_a = ref_a.signed_area();
    let area_b = ref_b.signed_area();
    let area_tol = 1e-9 * sim.scale * sim.scale + 1e-6 * area_a.abs();
    if (area_a - area_b).abs() > area_tol {
        failures.push(format!(
            "{label}: independent area differs fresh={area_a:.9} reused={area_b:.9} \
             delta={:.3e}",
            (area_a - area_b).abs()
        ));
    }

    let grid = probe_grid(&source, 2.6);
    for &p in &grid {
        let inside_a = ref_a.winding(p) != 0;
        let inside_b = ref_b.winding(p) != 0;
        if inside_a != inside_b {
            failures.push(format!(
                "{label}: membership differs at ({:.6},{:.6}) fresh_inside={inside_a} \
                 reused_inside={inside_b}",
                p.x, p.y
            ));
            break;
        }
    }
    failures
}

/// Compare the transformed output against the identity output carried through the same
/// similarity transform. Vertex order is never compared: topology counts, independent signed
/// area and region membership are.
pub fn compare_transformed_to_base(
    fx: &Fixture,
    sim: &Sim,
    base: &Shape<f64>,
    transformed: &Shape<f64>,
    label: &str,
) -> Vec<String> {
    let base_ref = sim.transform_shape(&output_reference(base));
    let actual = output_reference(transformed);
    let mut failures = Vec::new();

    let count_sign = |shape: &RefShape| {
        let mut components = 0usize;
        let mut holes = 0usize;
        for l in &shape.loops {
            if l.signed_area() > 0.0 {
                components += 1;
            } else {
                holes += 1;
            }
        }
        (components, holes)
    };
    let (ca, ha) = count_sign(&base_ref);
    let (cb, hb) = count_sign(&actual);
    if (ca, ha) != (cb, hb) {
        failures.push(format!(
            "{label}: topology differs transformed_base components={ca} holes={ha} \
             actual components={cb} holes={hb}"
        ));
    }

    let expected_area = base_ref.signed_area();
    let actual_area = actual.signed_area();
    let tol = 1e-8 * sim.scale * sim.scale + 1e-6 * expected_area.abs();
    if (expected_area - actual_area).abs() > tol {
        failures.push(format!(
            "{label}: independent area differs transformed_base={expected_area:.9} \
             actual={actual_area:.9} delta={:.3e}",
            (expected_area - actual_area).abs()
        ));
    }

    // Membership on the transformed probe grid, guarded near the analytic target boundary.
    let input = build_input(fx, sim);
    let source = RefShape::new(input.iter().map(RefLoop::from_pline).collect());
    let guard = GUARD_BASE * sim.scale;
    // Recover the offset value from the transformed result is not possible; use the caller's
    // fixture case through the boundary itself. Compare the two regions only where both are
    // decisively inside or outside, skipping points near either region boundary.
    let grid = probe_grid(&source, 2.6);
    for &p in &grid {
        if expected_area == 0.0 && base_ref.loops.is_empty() {
            if !actual.loops.is_empty() {
                failures.push(format!(
                    "{}: expected empty region but transformed output has {} loops",
                    label,
                    actual.loops.len()
                ));
            }
            break;
        }
        let a_inside = base_ref.winding(p) != 0;
        let b_inside = actual.winding(p) != 0;
        let near = base_ref.boundary_distance(p) <= guard || actual.boundary_distance(p) <= guard;
        if near {
            continue;
        }
        if a_inside != b_inside {
            failures.push(format!(
                "{label}: membership differs at ({:.6},{:.6}) transformed_base_inside={a_inside} \
                 actual_inside={b_inside}",
                p.x, p.y
            ));
            break;
        }
    }
    failures
}

/// Verify that the fixture's transformed input restored outer ccw / hole cw orientation and that
/// `Shape::from_plines` classifies it accordingly.
pub fn check_input_orientation(fx: &Fixture, run: &OffsetRun, sim: &Sim) -> Vec<String> {
    let mut failures = Vec::new();
    let base = RefShape::new(fx.plines.iter().map(RefLoop::from_pline).collect());
    let transformed = source_reference(run);
    if base.loops.len() != transformed.loops.len() {
        failures.push(format!(
            "loop count changed by transform base={} transformed={}",
            base.loops.len(),
            transformed.loops.len()
        ));
        return failures;
    }
    for (idx, (b, t)) in base.loops.iter().zip(transformed.loops.iter()).enumerate() {
        let expected_sign = if b.signed_area() > 0.0 { 1.0 } else { -1.0 };
        let area_t = t.signed_area();
        if area_t * expected_sign <= 0.0 {
            failures.push(format!(
                "transformed loop {idx} has signed area {area_t:.6e} after {} \
                 (expected sign {:+.0})",
                sim.label(),
                expected_sign
            ));
        }
        let expected_mag = b.signed_area().abs() * sim.scale * sim.scale;
        if (area_t.abs() - expected_mag).abs() > 1e-9 * sim.scale * sim.scale + 1e-6 * expected_mag
        {
            failures.push(format!(
                "transformed loop {idx} area magnitude {:.6e} != scaled base {:.6e}",
                area_t.abs(),
                expected_mag
            ));
        }
    }

    let expected_ccw = base.loops.iter().filter(|l| l.signed_area() > 0.0).count();
    let expected_cw = base.loops.len() - expected_ccw;
    if run.shape.ccw_plines.len() != expected_ccw || run.shape.cw_plines.len() != expected_cw {
        failures.push(format!(
            "Shape::from_plines classification ccw={} cw={} expected ccw={expected_ccw} \
             cw={expected_cw}",
            run.shape.ccw_plines.len(),
            run.shape.cw_plines.len()
        ));
    }
    failures
}

/// Bit snapshot of a fixture's original input loops.
pub fn fixture_bits(fx: &Fixture) -> Vec<Vec<u64>> {
    super::geometry::plines_bits(&fx.plines)
}

/// Input byte level data must be unchanged: the snapshot taken before the offset must equal the
/// retained transformed input after the offset, and a fresh rebuild of the transformed input must
/// produce the same bytes. This catches both mutation of retained loop data and nondeterminism in
/// the transform.
pub fn check_input_bits(fx: &Fixture, sim: &Sim, run: &OffsetRun) -> Vec<String> {
    let mut failures = Vec::new();
    let after = super::geometry::plines_bits(&run.input);
    if after != run.input_bits {
        failures.push(format!(
            "input polyline bytes changed after offset for {} under {}",
            fx.name,
            sim.label()
        ));
    }
    let rebuilt = build_input(fx, sim);
    if super::geometry::plines_bits(&rebuilt) != run.input_bits {
        failures.push(format!(
            "fresh transform rebuild produced different input bytes for {} under {}",
            fx.name,
            sim.label()
        ));
    }
    failures
}
