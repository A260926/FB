//! Independent geometry regression for `Shape::parallel_offset` on closed contours with holes.
//!
//! Expected values are produced only by the support module in `offset_reference_support`, which
//! stores its own directed lines and arcs and computes distances, horizontal ray crossings,
//! winding numbers and signed areas in double precision. It never calls the library's
//! `closest_point`, `winding_number`, `seg_arc_radius_and_center` or `area`, and never inspects
//! offset intermediates. The library is only exercised through `Shape::from_plines` and
//! `Shape::parallel_offset`.

#[path = "offset_reference_support/mod.rs"]
mod support;

use cavalier_contours::shape_algorithms::Shape;
use support::fixtures::{self, Fixture};
use support::geometry::{RefLoop, RefShape, Sim};
use support::harness::{self, ASSERT_MARKER};

/// Additional similarity transforms beyond plain rotation / translation / scale / mirror.
/// Reflection is composed with rotation and a uniform scale.
fn transforms() -> Vec<Sim> {
    vec![
        Sim::rotation(0.7, 12.0, -8.0),
        Sim::translation(-31.0, 17.0),
        Sim::uniform_scale(2.5),
        Sim::uniform_scale(0.6),
        Sim::mirror_y_equals_x(9.0, -4.0),
        Sim::mirror_y_equals_x(0.0, 0.0),
        Sim {
            cos: 0.8,
            sin: 0.6,
            scale: 0.6,
            tx: 4.0,
            ty: -6.0,
            mirror: true,
        },
        Sim {
            cos: 0.8,
            sin: 0.6,
            scale: 2.5,
            tx: 4.0,
            ty: -6.0,
            mirror: true,
        },
    ]
}

/// Verify the independent module on analytic calibration points for every fixture.
fn check_calibration(fx: &Fixture) -> Vec<String> {
    let source = RefShape::new(fx.plines.iter().map(RefLoop::from_pline).collect());
    let mut failures = Vec::new();
    // Fixtures must be wound as regions: every positive loop encloses and every hole subtracts.
    for (idx, lp) in source.loops.iter().enumerate() {
        if lp.signed_area() == 0.0 {
            failures.push(format!("{} loop {idx} has zero independent area", fx.name));
        }
    }
    for &(p, expected) in &fx.calibration {
        let actual = source.boundary_distance(p);
        let delta = (actual - expected).abs();
        if delta > 1e-9 {
            failures.push(format!(
                "{} calibration point ({:.6},{:.6}) distance {:.9} expected {:.9}",
                fx.name, p.x, p.y, actual, expected
            ));
        }
    }
    if let Some(expected) = fx.base_area {
        let actual = source.signed_area();
        let delta = (actual - expected).abs();
        if delta > 1e-9 * expected.abs().max(1.0) {
            failures.push(format!(
                "{} base independent area {:.9} expected analytic {:.9}",
                fx.name, actual, expected
            ));
        }
    }
    failures
}

/// Compare a fresh rebuild with a reused `Shape` across interleaved positive and negative offsets.
fn check_reuse_equivalence(fx: &Fixture, sim: &Sim) -> Vec<String> {
    let input = harness::build_input(fx, sim);
    let opts = harness::scaled_options(sim.scale);
    let reused = Shape::from_plines(input.clone());

    // Any repeated call on the same shape must be identical to a freshly built shape, independent
    // of vertex order and of which offsets were executed before it.
    let calls: Vec<f64> = fx
        .cases
        .iter()
        .map(|c| c.offset * sim.scale)
        .cycle()
        .take(fx.cases.len() * 2)
        .collect();
    // Reused shape: interleaved positive and negative offsets on one `Shape`.
    let interleaved: Vec<Shape<f64>> = calls
        .iter()
        .map(|&offset| reused.parallel_offset(offset, &opts))
        .collect();
    // Fresh rebuild for every call.
    let fresh: Vec<Shape<f64>> = calls
        .iter()
        .map(|&offset| Shape::from_plines(input.clone()).parallel_offset(offset, &opts))
        .collect();

    let mut failures = Vec::new();
    let mut last_offset: Option<f64> = None;
    for (idx, (reused_result, fresh_result)) in interleaved.iter().zip(fresh.iter()).enumerate() {
        let offset = calls[idx];
        let label = format!(
            "reuse fixture={} transform={} offset={} call={idx} previous_offset={:?}",
            fx.name,
            sim.label(),
            offset,
            last_offset
        );
        last_offset = Some(offset);
        failures.extend(harness::semantic_diff(
            fx,
            sim,
            fresh_result,
            reused_result,
            &label,
        ));
    }
    failures
}

#[test]
#[allow(clippy::assertions_on_constants)]
fn offset_metric_reference() {
    // The protection band is capped at 1e-3 original length units and is not widened to hide
    // geometry errors.
    assert!(
        harness::GUARD_BASE <= 1e-3,
        "{ASSERT_MARKER} guard band {} exceeds 1e-3 base units",
        harness::GUARD_BASE
    );
    let mut failures = Vec::new();
    for fx in fixtures::all() {
        failures.extend(check_calibration(&fx));
        let sim = Sim::identity();
        for case in &fx.cases {
            let run = harness::run_offset(&fx, &sim, case.offset);
            let report = harness::evaluate(&fx, case, &sim, &run);
            report.print();
            report.assert_ok();
        }
    }
    assert!(
        failures.is_empty(),
        "{ASSERT_MARKER} metric calibration failed: {failures:?}"
    );
}

#[test]
fn offset_topology_reference() {
    for fx in fixtures::all() {
        assert!(
            !fx.cases.is_empty(),
            "{ASSERT_MARKER} fixture {} has no cases",
            fx.name
        );
        let sim = Sim::identity();
        for case in &fx.cases {
            let run = harness::run_offset(&fx, &sim, case.offset);
            let report = harness::evaluate(&fx, case, &sim, &run);
            report.print();
            report.assert_ok();
        }
    }
}

#[test]
fn offset_transform_reference() {
    for fx in fixtures::all() {
        let identity = Sim::identity();
        for case in &fx.cases {
            let identity_run = harness::run_offset(&fx, &identity, case.offset);
            let identity_report = harness::evaluate(&fx, case, &identity, &identity_run);
            identity_report.print();
            identity_report.assert_ok();

            for sim in transforms() {
                let run = harness::run_offset(&fx, &sim, case.offset);
                let orientation_failures = harness::check_input_orientation(&fx, &run, &sim);
                let bits_failures = harness::check_input_bits(&fx, &sim, &run);
                let reuse_failures = check_reuse_equivalence(&fx, &sim);
                let transform_failures = harness::compare_transformed_to_base(
                    &fx,
                    &sim,
                    &identity_run.result,
                    &run.result,
                    &format!("transform fixture={} sim={}", fx.name, sim.label()),
                );
                let mut report = harness::evaluate(&fx, case, &sim, &run);
                report.failures.extend(orientation_failures);
                report.failures.extend(bits_failures);
                report.failures.extend(reuse_failures);
                report.failures.extend(transform_failures);
                report.print();
                report.assert_ok();
            }
        }
    }
}
