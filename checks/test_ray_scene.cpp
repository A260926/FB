/**
 * Task checks for the direct rational-surface ray scene.
 *
 * Three groups are registered with the task driver:
 *   roots   - analytic transverse queries, ordering, knot-line conventions,
 *             tangency and input rejection
 *   updates - batched control point edits, sparse re-extraction, rejection
 *             and ownership/lifecycle guarantees
 *   bounds  - hierarchy pruning, real visit counters, budget handling and
 *             construction validation
 *
 * This file defines no main(); task_support/driver.c provides it.
 */

#include <tinynurbs/tinynurbs.h>
#include "task_support/check.h"

#include <cmath>
#include <vector>

using tinynurbs::RationalSurface3d;
using tinynurbs::RaySurfaceScene;
using tinynurbs::RayOptions;
using tinynurbs::RaySceneError;
using tinynurbs::RaySceneUpdateStats;
using tinynurbs::ControlPointEdit;
using tinynurbs::UnresolvedReason;
using glm::dvec3;

namespace
{

constexpr double kSqrtHalf = 0.70710678118654752440;

RayOptions strictOptions()
{
    RayOptions opt;
    opt.spatial_tolerance = 1e-8;
    opt.parameter_tolerance = 1e-9;
    opt.box_budget = 100000;
    return opt;
}

// S(u,v) = (u, v, u*v), the folded bilinear reference patch.
RationalSurface3d makeBilinear()
{
    RationalSurface3d s;
    s.degree_u = 1;
    s.degree_v = 1;
    s.knots_u = {0, 0, 1, 1};
    s.knots_v = {0, 0, 1, 1};
    s.control_points = {2, 2,
                        {dvec3(0, 0, 0), dvec3(0, 1, 0),
                         dvec3(1, 0, 0), dvec3(1, 1, 1)}};
    s.weights = {2, 2, {1, 1, 1, 1}};
    return s;
}

// Unit-radius quarter cylinder band, rational weights, arc 0..pi/2, z in [0,2].
RationalSurface3d makeQuarterCylinder()
{
    RationalSurface3d s;
    s.degree_u = 2;
    s.degree_v = 1;
    const double w = std::sqrt(0.5);
    s.knots_u = {0, 0, 0, 1, 1, 1};
    s.knots_v = {0, 0, 1, 1};
    s.control_points = {3, 2,
                        {dvec3(1, 0, 0), dvec3(1, 0, 2),
                         dvec3(1, 1, 0), dvec3(1, 1, 2),
                         dvec3(0, 1, 0), dvec3(0, 1, 2)}};
    s.weights = {3, 2, {1, 1, w, w, 1, 1}};
    return s;
}

// Rational plane over a deliberately non-unit parameter domain.
RationalSurface3d makeRationalPlane()
{
    RationalSurface3d s;
    s.degree_u = 1;
    s.degree_v = 1;
    s.knots_u = {0, 0, 2, 2};
    s.knots_v = {-3, -3, 5, 5};
    s.control_points = {2, 2,
                        {dvec3(0, 0, 1), dvec3(0, 4, 1),
                         dvec3(2, 0, 1), dvec3(2, 4, 1)}};
    s.weights = {2, 2, {1, 2, 3, 4}};
    return s;
}

// Degree-2 ribbon folded back over itself with a C0 crease at u = 1; the two
// elements share spatial points but have opposite Su x Sv normals.
RationalSurface3d makeFoldedRibbon()
{
    RationalSurface3d s;
    s.degree_u = 2;
    s.degree_v = 1;
    s.knots_u = {0, 0, 0, 1, 1, 2, 2, 2}; // interior multiplicity equals degree
    s.knots_v = {0, 0, 1, 1};
    s.control_points = {5, 2,
                        {dvec3(0, 0, 0), dvec3(0, 1, 0),
                         dvec3(0.5, 0, 0), dvec3(0.5, 1, 0),
                         dvec3(1, 0, 0), dvec3(1, 1, 0),
                         dvec3(0.5, 0, 0), dvec3(0.5, 1, 0),
                         dvec3(0, 0, 0), dvec3(0, 1, 0)}};
    s.weights = {5, 2, std::vector<double>(10, 1.0)};
    return s;
}

// Two-element degree-2 surface, z = 0, used for edit support checks.
RationalSurface3d makeTwoElementPatch()
{
    RationalSurface3d s;
    s.degree_u = 2;
    s.degree_v = 1;
    s.knots_u = {0, 0, 0, 1, 2, 2, 2};
    s.knots_v = {0, 0, 1, 1};
    s.control_points = {4, 2,
                        {dvec3(0, 0, 0), dvec3(0, 1, 0),
                         dvec3(1, 0, 0), dvec3(1, 1, 0),
                         dvec3(2, 0, 0), dvec3(2, 1, 0),
                         dvec3(3, 0, 0), dvec3(3, 1, 0)}};
    s.weights = {4, 2, std::vector<double>(8, 1.0)};
    return s;
}

// Rational cubic Bezier surface, covering the degree-3 supported class.
RationalSurface3d makeCubicBezier()
{
    RationalSurface3d s;
    s.degree_u = 3;
    s.degree_v = 3;
    s.knots_u = {0, 0, 0, 0, 1, 1, 1, 1};
    s.knots_v = {0, 0, 0, 0, 1, 1, 1, 1};
    s.control_points.resize(4, 4);
    s.weights.resize(4, 4);
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
        {
            s.control_points(i, j) =
                dvec3(i, j, 0.25 * i * i - 0.1 * j * j + 0.05 * i * j);
            s.weights(i, j) = 1.0 + 0.1 * i - 0.05 * j;
        }
    return s;
}

bool close(double a, double b, double eps) { return std::abs(a - b) <= eps; }

// ---------------------------------------------------------------------------
// roots group
// ---------------------------------------------------------------------------

int rootsBilinearDouble()
{
    RaySurfaceScene scene({makeBilinear()});
    TASK_CHECK(scene.status() == RaySceneError::None);
    const RayOptions opt = strictOptions();
    auto r = scene.intersect(dvec3(0, 0, -0.16), dvec3(1, 1, 1), -10.0, 10.0, opt);
    TASK_CHECK(r.error == RaySceneError::None);
    TASK_CHECK(r.complete);
    TASK_CHECK(r.hits.size() == 2);
    TASK_CHECK(r.unresolved.empty());
    // Sorted by t.
    TASK_CHECK(r.hits[0].t <= r.hits[1].t);
    // Reference roots t = 0.2 and t = 0.8 with u = v = t.
    TASK_CHECK(close(r.hits[0].t, 0.2, 1e-8));
    TASK_CHECK(close(r.hits[1].t, 0.8, 1e-8));
    for (const auto &h : r.hits)
    {
        TASK_CHECK(h.surface_index == 0);
        TASK_CHECK(close(h.u, h.t, 1e-8));
        TASK_CHECK(close(h.v, h.t, 1e-8));
        const dvec3 ray_point = dvec3(0, 0, -0.16) + h.t * dvec3(1, 1, 1);
        TASK_CHECK(glm::length(h.position - ray_point) <= 1e-8);
        TASK_CHECK(close(glm::length(h.normal), 1.0, 1e-12));
        // Normal follows Su x Sv of (u, v, uv): (-v, -u, 1).
        dvec3 ref = dvec3(-h.v, -h.u, 1.0);
        ref /= glm::length(ref);
        TASK_CHECK(glm::length(h.normal - ref) <= 1e-10);
    }
    return 0;
}

int rootsCylinderAnalytic()
{
    RaySurfaceScene scene({makeQuarterCylinder()});
    TASK_CHECK(scene.status() == RaySceneError::None);
    const RayOptions opt = strictOptions();
    const dvec3 O(kSqrtHalf, -2.0, 1.0), D(0, 1, 0);
    auto r = scene.intersect(O, D, 0.0, 10.0, opt);
    TASK_CHECK(r.complete);
    TASK_CHECK(r.hits.size() == 1);
    const auto &h = r.hits[0];
    TASK_CHECK(close(h.t, 2.0 + kSqrtHalf, 1e-8));
    TASK_CHECK(close(h.u, 0.5, 1e-9));
    TASK_CHECK(close(h.v, 0.5, 1e-9));
    TASK_CHECK(close(h.position.x, kSqrtHalf, 1e-8));
    TASK_CHECK(close(h.position.y, kSqrtHalf, 1e-8));
    TASK_CHECK(close(h.position.z, 1.0, 1e-8));
    TASK_CHECK(glm::length(h.normal - dvec3(kSqrtHalf, kSqrtHalf, 0)) <= 1e-10);
    TASK_CHECK(glm::length(h.position - (O + h.t * D)) <= 1e-8);
    return 0;
}

int rootsPlaneDomainAndWindow()
{
    RaySurfaceScene scene({makeRationalPlane()});
    TASK_CHECK(scene.status() == RaySceneError::None);
    const RayOptions opt = strictOptions();
    const dvec3 O(0.5, 1.0, -5.0), D(0, 0, 1);
    auto r = scene.intersect(O, D, 0.0, 100.0, opt);
    TASK_CHECK(r.complete);
    TASK_CHECK(r.hits.size() == 1);
    TASK_CHECK(close(r.hits[0].t, 6.0, 1e-8));
    TASK_CHECK(close(r.hits[0].position.z, 1.0, 1e-8));
    // Hit parameters stay inside the non-unit domain.
    TASK_CHECK(r.hits[0].u >= 0.0 && r.hits[0].u <= 2.0);
    TASK_CHECK(r.hits[0].v >= -3.0 && r.hits[0].v <= 5.0);
    // Half-open finite window: [0, 6) excludes the root, [6, 100) keeps it.
    auto before = scene.intersect(O, D, 0.0, 6.0, opt);
    auto at = scene.intersect(O, D, 6.0, 100.0, opt);
    TASK_CHECK(before.complete && before.hits.empty());
    TASK_CHECK(at.complete && at.hits.size() == 1);
    return 0;
}

int rootsSamePointDifferentUV()
{
    RaySurfaceScene scene({makeFoldedRibbon()});
    TASK_CHECK(scene.status() == RaySceneError::None);
    const RayOptions opt = strictOptions();
    const dvec3 hit_point = tinynurbs::surfacePoint(makeFoldedRibbon(), 0.5, 0.5);
    auto r = scene.intersect(hit_point + dvec3(0, 0, -5), dvec3(0, 0, 1), 0.0, 100.0, opt);
    TASK_CHECK(r.complete);
    TASK_CHECK(r.hits.size() == 2); // distinct UV roots are both retained
    TASK_CHECK(close(r.hits[0].t, r.hits[1].t, 1e-10));
    TASK_CHECK(glm::length(r.hits[0].position - r.hits[1].position) <= 1e-10);
    TASK_CHECK(std::abs(r.hits[0].u - r.hits[1].u) > 0.1);
    // Opposite orientations on the two folded elements.
    TASK_CHECK(glm::length(r.hits[0].normal + r.hits[1].normal) <= 1e-10);
    TASK_CHECK(close(glm::length(r.hits[0].normal), 1.0, 1e-12));
    return 0;
}

int rootsKnotLineAndUpperBoundNormals()
{
    const auto srf = makeTwoElementPatch();
    RaySurfaceScene scene({srf});
    const RayOptions opt = strictOptions();
    // Interior knot u = 1: the right element defines the normal.
    dvec3 P = tinynurbs::surfacePoint(srf, 1.0, 0.5);
    auto r = scene.intersect(P + dvec3(0, 0, -1), dvec3(0, 0, 1), 0.0, 10.0, opt);
    TASK_CHECK(r.complete);
    TASK_CHECK(r.hits.size() == 1); // no duplicate across the knot line
    TASK_CHECK(close(r.hits[0].u, 1.0, 1e-9));
    auto ders = tinynurbs::surfaceDerivatives(srf, 1, 1.0, 0.5);
    dvec3 ref = glm::cross(ders(1, 0), ders(0, 1));
    ref /= glm::length(ref);
    TASK_CHECK(glm::length(r.hits[0].normal - ref) <= 1e-10);
    // Upper domain bound u = 2: the left (last) element defines the normal.
    dvec3 Q = tinynurbs::surfacePoint(srf, 2.0, 0.5);
    auto r2 = scene.intersect(Q + dvec3(0, 0, -1), dvec3(0, 0, 1), 0.0, 10.0, opt);
    TASK_CHECK(r2.complete && r2.hits.size() == 1);
    TASK_CHECK(close(r2.hits[0].u, 2.0, 1e-9));
    auto ders2 = tinynurbs::surfaceDerivatives(srf, 1, 2.0, 0.5);
    dvec3 ref2 = glm::cross(ders2(1, 0), ders2(0, 1));
    ref2 /= glm::length(ref2);
    TASK_CHECK(glm::length(r2.hits[0].normal - ref2) <= 1e-10);
    return 0;
}

int rootsTangencyIsUnresolved()
{
    RaySurfaceScene scene({makeBilinear()});
    const RayOptions opt = strictOptions();
    // The ray z = 0 grazes the whole v = 0 edge of the patch.
    auto r = scene.intersect(dvec3(0, 0, 0), dvec3(1, 0, 0), 0.0, 10.0, opt);
    TASK_CHECK(!r.complete);
    TASK_CHECK(!r.unresolved.empty());
    for (const auto &u : r.unresolved)
    {
        TASK_CHECK(u.surface_index == 0);
        TASK_CHECK(u.u_max >= u.u_min && u.v_max >= u.v_min);
        // A tangent contact must be explicitly reported, never silently
        // treated as a miss or claimed as an unverified hit.
        TASK_CHECK(u.reason == UnresolvedReason::Tangency ||
                   u.reason == UnresolvedReason::Degenerate ||
                   u.reason == UnresolvedReason::Budget);
    }
    return 0;
}

int rootsCubicRational()
{
    const auto srf = makeCubicBezier();
    RaySurfaceScene scene({srf});
    TASK_CHECK(scene.status() == RaySceneError::None);
    TASK_CHECK(scene.leafCount() == 1);
    const RayOptions opt = strictOptions();
    const double u0 = 0.37, v0 = 0.63;
    const dvec3 P = tinynurbs::surfacePoint(srf, u0, v0);
    auto r = scene.intersect(P + dvec3(0, 0, -5), dvec3(0, 0, 1), 0.0, 100.0, opt);
    TASK_CHECK(r.complete);
    TASK_CHECK(r.hits.size() == 1);
    TASK_CHECK(close(r.hits[0].u, u0, 1e-8));
    TASK_CHECK(close(r.hits[0].v, v0, 1e-8));
    TASK_CHECK(glm::length(r.hits[0].position - P) <= 1e-8);
    TASK_CHECK(close(glm::length(r.hits[0].normal), 1.0, 1e-12));
    auto ders = tinynurbs::surfaceDerivatives(srf, 1, u0, v0);
    dvec3 ref = glm::cross(ders(1, 0), ders(0, 1));
    ref /= glm::length(ref);
    TASK_CHECK(glm::length(r.hits[0].normal - ref) <= 1e-10);
    return 0;
}

int rootsInfiniteCoincidenceUnresolved()
{
    RaySurfaceScene scene({makeRationalPlane()});
    const RayOptions opt = strictOptions();
    // A ray lying entirely in the plane z = 1: infinitely many coincidences,
    // which must not be returned as discrete verified hits.
    auto r = scene.intersect(dvec3(0.5, 1.0, 1.0), dvec3(1.0, 0.0, 0.0), 0.0, 1.5, opt);
    TASK_CHECK(!r.complete);
    TASK_CHECK(!r.unresolved.empty());
    for (const auto &h : r.hits)
    {
        // Any retained contact must still satisfy the spatial tolerance.
        const dvec3 ray_point = dvec3(0.5, 1.0, 1.0) + h.t * dvec3(1, 0, 0);
        TASK_CHECK(glm::length(h.position - ray_point) <= 1e-8);
    }
    return 0;
}

int rootsRejectBadQueries()
{
    RaySurfaceScene scene({makeBilinear()});
    const RayOptions opt = strictOptions();
    auto zero_dir = scene.intersect(dvec3(0), dvec3(0), 0, 1, opt);
    TASK_CHECK(zero_dir.error == RaySceneError::InvalidRay);
    auto nonfinite = scene.intersect(dvec3(0), dvec3(std::nan(""), 1, 0), 0, 1, opt);
    TASK_CHECK(nonfinite.error == RaySceneError::InvalidRay);
    auto bad_interval = scene.intersect(dvec3(0), dvec3(1, 0, 0), 1.0, 1.0, opt);
    TASK_CHECK(bad_interval.error == RaySceneError::InvalidInterval);
    RayOptions bad_tol = opt;
    bad_tol.spatial_tolerance = 0.0;
    auto zero_tol = scene.intersect(dvec3(0), dvec3(1, 0, 0), 0, 1, bad_tol);
    TASK_CHECK(zero_tol.error == RaySceneError::InvalidTolerance);
    RayOptions bad_budget = opt;
    bad_budget.box_budget = 0;
    auto zero_budget = scene.intersect(dvec3(0), dvec3(1, 0, 0), 0, 1, bad_budget);
    TASK_CHECK(zero_budget.error == RaySceneError::InvalidBudget);
    // Rejected queries do no work and leave no partial results.
    TASK_CHECK(zero_dir.boxes_visited == 0 && zero_dir.hits.empty());
    return 0;
}

// ---------------------------------------------------------------------------
// updates group
// ---------------------------------------------------------------------------

int updatesSparseExtraction()
{
    RaySurfaceScene scene({makeTwoElementPatch()});
    TASK_CHECK(scene.status() == RaySceneError::None);
    TASK_CHECK(scene.leafCount() == 2);
    TASK_CHECK(scene.elementCount(0) == 2);
    const std::size_t nodes_before = scene.nodeCount();
    const unsigned long long extracted_before = scene.totalElementsExtracted();
    // Control row 0 only supports the first knot element (span 0).
    RaySceneUpdateStats st = scene.updateControlPoints(
        {{0, 0, 0, dvec3(0, 0, 0.5), 1.0}});
    TASK_CHECK(st.accepted);
    TASK_CHECK(st.elements_extracted == 1);
    TASK_CHECK(st.bvh_nodes_refit >= 1);
    TASK_CHECK(scene.totalElementsExtracted() == extracted_before + 1);
    // Control row 2 supports both elements.
    st = scene.updateControlPoints({{0, 2, 0, dvec3(2, 0, 0.25), 1.0}});
    TASK_CHECK(st.accepted);
    TASK_CHECK(st.elements_extracted == 2);
    TASK_CHECK(scene.totalElementsExtracted() == extracted_before + 3);
    // Topology and leaf numbering are preserved.
    TASK_CHECK(scene.nodeCount() == nodes_before);
    TASK_CHECK(scene.leafCount() == 2);
    // The query reflects the absolute new weight and position: at u = v = 0
    // the surface equals control point (0, 0), whose z was raised to 0.5.
    const RayOptions opt = strictOptions();
    auto r = scene.intersect(dvec3(0, 0, -1), dvec3(0, 0, 1), 0, 10, opt);
    TASK_CHECK(r.complete && r.hits.size() == 1);
    TASK_CHECK(close(r.hits[0].position.z, 0.5, 1e-8));
    return 0;
}

int updatesAbsoluteRepeatedEdits()
{
    RaySurfaceScene scene({makeTwoElementPatch()});
    const RayOptions opt = strictOptions();
    // Two sequential batches touching the same control point must keep the
    // latest absolute position and weight, not accumulate deltas.
    RaySceneUpdateStats s1 = scene.updateControlPoints(
        {{0, 0, 0, dvec3(0, 0, 0.4), 2.0}});
    TASK_CHECK(s1.accepted);
    RaySceneUpdateStats s2 = scene.updateControlPoints(
        {{0, 0, 0, dvec3(0, 0, 0.9), 3.0}});
    TASK_CHECK(s2.accepted);
    auto r = scene.intersect(dvec3(0, 0, -1), dvec3(0, 0, 1), 0, 10, opt);
    TASK_CHECK(r.complete && r.hits.size() == 1);
    TASK_CHECK(close(r.hits[0].position.z, 0.9, 1e-8));
    // Weight changed too: a non-unit weight at a corner still equals the
    // weighted control point after perspective division.
    TASK_CHECK(close(r.hits[0].position.x, 0.0, 1e-12));
    TASK_CHECK(close(r.hits[0].position.y, 0.0, 1e-12));
    return 0;
}

int updatesRejectThenRetry()
{
    RaySurfaceScene scene({makeTwoElementPatch()});
    const RayOptions opt = strictOptions();
    const auto before =
        scene.intersect(dvec3(0, 0, -1), dvec3(0, 0, 1), 0, 10, opt);
    TASK_CHECK(before.complete && before.hits.size() == 1);
    const unsigned long long extracted_before = scene.totalElementsExtracted();
    // Duplicate address rejects the whole batch.
    RaySceneUpdateStats dup = scene.updateControlPoints(
        {{0, 0, 0, dvec3(0, 0, 1), 1.0}, {0, 0, 0, dvec3(0, 0, 2), 2.0}});
    TASK_CHECK(!dup.accepted);
    TASK_CHECK(dup.error == RaySceneError::DuplicateControlPoint);
    TASK_CHECK(dup.elements_extracted == 0);
    // Illegal values reject the whole batch.
    RaySceneUpdateStats bad_w = scene.updateControlPoints(
        {{0, 0, 0, dvec3(0, 0, 1), -0.5}});
    TASK_CHECK(!bad_w.accepted && bad_w.error == RaySceneError::InvalidControlPoint);
    RaySceneUpdateStats bad_idx = scene.updateControlPoints(
        {{9, 0, 0, dvec3(0, 0, 1), 1.0}});
    TASK_CHECK(!bad_idx.accepted && bad_idx.error == RaySceneError::InvalidControlPoint);
    TASK_CHECK(scene.totalElementsExtracted() == extracted_before);
    // The scene is still queryable and unchanged.
    auto unchanged = scene.intersect(dvec3(0, 0, -1), dvec3(0, 0, 1), 0, 10, opt);
    TASK_CHECK(unchanged.complete && unchanged.hits.size() == 1);
    TASK_CHECK(close(unchanged.hits[0].position.z, before.hits[0].position.z, 1e-14));
    // A valid retry now succeeds and takes effect.
    RaySceneUpdateStats ok = scene.updateControlPoints(
        {{0, 0, 0, dvec3(0, 0, 0.7), 1.5}});
    TASK_CHECK(ok.accepted && ok.error == RaySceneError::None);
    auto after = scene.intersect(dvec3(0, 0, -1), dvec3(0, 0, 1), 0, 10, opt);
    TASK_CHECK(after.complete && close(after.hits[0].position.z, 0.7, 1e-8));
    return 0;
}

int updatesEmptyAndLifecycle()
{
    std::vector<RationalSurface3d> source = {makeTwoElementPatch()};
    RaySurfaceScene scene(source);
    // Mutating and destroying the source must not affect the owned scene.
    source[0].control_points(0, 0) = dvec3(99, 99, 99);
    source.clear();
    const RayOptions opt = strictOptions();
    auto baseline = scene.intersect(dvec3(0, 0, -1), dvec3(0, 0, 1), 0, 10, opt);
    TASK_CHECK(baseline.complete && baseline.hits.size() == 1);
    // An empty batch performs no extraction.
    const unsigned long long extracted_before = scene.totalElementsExtracted();
    RaySceneUpdateStats empty = scene.updateControlPoints({});
    TASK_CHECK(empty.accepted);
    TASK_CHECK(empty.elements_extracted == 0 && empty.bvh_nodes_refit == 0);
    TASK_CHECK(scene.totalElementsExtracted() == extracted_before);
    // The edit list is only borrowed for the call.
    std::vector<ControlPointEdit> edits = {{0, 0, 0, dvec3(0, 0, 0.3), 1.0}};
    RaySceneUpdateStats st = scene.updateControlPoints(edits);
    TASK_CHECK(st.accepted);
    edits.clear();
    edits.push_back({0, 0, 0, dvec3(7, 7, 7), 9.0});
    auto after = scene.intersect(dvec3(0, 0, -1), dvec3(0, 0, 1), 0, 10, opt);
    TASK_CHECK(after.complete && close(after.hits[0].position.z, 0.3, 1e-8));
    // Old query results own their data and survive further updates.
    const double old_t = baseline.hits[0].t;
    scene.updateControlPoints({{0, 1, 0, dvec3(1, 0, 0.4), 1.0}});
    TASK_CHECK(close(baseline.hits[0].t, old_t, 0.0));
    TASK_CHECK(baseline.hits[0].position.z == 0.0);
    return 0;
}

// ---------------------------------------------------------------------------
// bounds group
// ---------------------------------------------------------------------------

int boundsHierarchicalPruning()
{
    auto near_patch = makeBilinear();
    auto far_patch = makeBilinear();
    for (size_t i = 0; i < far_patch.control_points.size(); ++i)
        far_patch.control_points[i] += dvec3(1000, 0, 0);
    RaySurfaceScene scene({near_patch, far_patch});
    TASK_CHECK(scene.leafCount() == 2);
    TASK_CHECK(scene.nodeCount() == 3);
    const RayOptions opt = strictOptions();
    auto r = scene.intersect(dvec3(0, 0, -0.16), dvec3(1, 1, 1), -10, 10, opt);
    TASK_CHECK(r.complete);
    TASK_CHECK(r.hits.size() == 2);
    for (const auto &h : r.hits) TASK_CHECK(h.surface_index == 0);
    // The far subtree is pruned: root plus the near leaf only.
    TASK_CHECK(r.bvh_nodes_visited < scene.nodeCount());
    TASK_CHECK(r.bvh_nodes_visited == 2);
    TASK_CHECK(r.boxes_visited == r.bvh_nodes_visited + r.parameter_boxes_visited);
    // A ray missing everything is rejected at the root box before any node
    // or parameter box is opened.
    auto miss = scene.intersect(dvec3(-5000, 0, 0), dvec3(0, 0, 1), 0, 10, opt);
    TASK_CHECK(miss.complete && miss.hits.empty());
    TASK_CHECK(miss.bvh_nodes_visited == 0);
    TASK_CHECK(miss.parameter_boxes_visited == 0);
    TASK_CHECK(miss.boxes_visited == 0);
    return 0;
}

int boundsCountersAreReal()
{
    RaySurfaceScene scene({makeQuarterCylinder(), makeBilinear()});
    const RayOptions opt = strictOptions();
    auto hit = scene.intersect(dvec3(kSqrtHalf, -2, 1), dvec3(0, 1, 0), 0, 10, opt);
    TASK_CHECK(hit.complete && !hit.hits.empty());
    TASK_CHECK(hit.parameter_boxes_visited > 0);
    TASK_CHECK(hit.bvh_nodes_visited > 0);
    // A second, different ray on another scene produces its own live counts,
    // and counters are not constants (the miss case below also reaches zero).
    RaySurfaceScene bilinear_only({makeBilinear()});
    auto hit2 = bilinear_only.intersect(dvec3(0, 0, -0.16), dvec3(1, 1, 1), -10, 10, opt);
    TASK_CHECK(hit2.complete && hit2.hits.size() == 2);
    TASK_CHECK(hit2.parameter_boxes_visited > 0);
    TASK_CHECK(hit2.boxes_visited ==
               hit2.bvh_nodes_visited + hit2.parameter_boxes_visited);
    auto miss2 = bilinear_only.intersect(dvec3(-5000, 0, 0), dvec3(0, 0, 1), 0, 10, opt);
    TASK_CHECK(miss2.complete && miss2.boxes_visited == 0);
    return 0;
}

int boundsBudgetUnresolved()
{
    RaySurfaceScene scene({makeQuarterCylinder()});
    RayOptions tiny = strictOptions();
    tiny.box_budget = 3;
    auto r = scene.intersect(dvec3(kSqrtHalf, -2, 1), dvec3(0, 1, 0), 0, 10, tiny);
    TASK_CHECK(!r.complete);
    TASK_CHECK(r.parameter_boxes_visited <= tiny.box_budget);
    TASK_CHECK(!r.unresolved.empty());
    TASK_CHECK(r.unresolved.front().reason == UnresolvedReason::Budget);
    TASK_CHECK(r.unresolved.front().surface_index == 0);
    // The unresolved range encloses a real parameter region of the surface.
    TASK_CHECK(r.unresolved.front().u_min >= 0.0 &&
               r.unresolved.front().u_max <= 1.0);
    return 0;
}

int boundsConstructionValidation()
{
    // Unsupported degree.
    auto bad_degree = makeBilinear();
    bad_degree.degree_u = 4;
    RaySurfaceScene s1({bad_degree});
    TASK_CHECK(s1.status() == RaySceneError::InvalidSurface);
    const RayOptions opt = strictOptions();
    TASK_CHECK(s1.intersect(dvec3(0), dvec3(1, 0, 0), 0, 1, opt).error ==
               RaySceneError::InvalidSurface);
    // Non-clamped knot vector.
    auto bad_knots = makeBilinear();
    bad_knots.degree_u = 2;
    bad_knots.knots_u = {0, 0, 1, 1};
    RaySurfaceScene s2({bad_knots});
    TASK_CHECK(s2.status() == RaySceneError::InvalidSurface);
    // Non-positive weight.
    auto bad_weight = makeBilinear();
    bad_weight.weights(0, 0) = -1.0;
    RaySurfaceScene s3({bad_weight});
    TASK_CHECK(s3.status() == RaySceneError::InvalidSurface);
    // Interior knot multiplicity equal to the degree is supported.
    RaySurfaceScene s4({makeFoldedRibbon()});
    TASK_CHECK(s4.status() == RaySceneError::None);
    TASK_CHECK(s4.leafCount() == 2);
    // An empty scene is valid and answers empty queries.
    RaySurfaceScene empty(std::vector<RationalSurface3d>{});
    TASK_CHECK(empty.status() == RaySceneError::None);
    auto r = empty.intersect(dvec3(0), dvec3(1, 0, 0), 0, 1, opt);
    TASK_CHECK(r.error == RaySceneError::None && r.complete && r.hits.empty());
    return 0;
}

} // namespace

const struct task_case task_cases[] = {
    {"roots", "bilinear_double_roots", rootsBilinearDouble},
    {"roots", "cylinder_analytic", rootsCylinderAnalytic},
    {"roots", "cubic_rational", rootsCubicRational},
    {"roots", "plane_domain_and_window", rootsPlaneDomainAndWindow},
    {"roots", "same_point_distinct_uv", rootsSamePointDifferentUV},
    {"roots", "knot_line_normals", rootsKnotLineAndUpperBoundNormals},
    {"roots", "tangency_unresolved", rootsTangencyIsUnresolved},
    {"roots", "infinite_coincidence_unresolved", rootsInfiniteCoincidenceUnresolved},
    {"roots", "invalid_query_rejection", rootsRejectBadQueries},
    {"updates", "sparse_extraction", updatesSparseExtraction},
    {"updates", "absolute_repeated_edits", updatesAbsoluteRepeatedEdits},
    {"updates", "reject_then_retry", updatesRejectThenRetry},
    {"updates", "empty_edit_and_lifecycle", updatesEmptyAndLifecycle},
    {"bounds", "hierarchical_pruning", boundsHierarchicalPruning},
    {"bounds", "real_visit_counters", boundsCountersAreReal},
    {"bounds", "budget_unresolved", boundsBudgetUnresolved},
    {"bounds", "construction_validation", boundsConstructionValidation},
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
