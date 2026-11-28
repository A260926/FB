/**
 * Task-registered checks for the direct rational-surface ray scene.
 *
 * Three groups are registered through task_support/check.h:
 *   roots   - analytic references, ordering, knot normals, unresolved paths
 *   updates - batched control point edits, rejection, lifetime, support
 *   bounds  - hierarchical pruning and real per-query/per-edit counters
 *
 * This file defines no main(); task_support/driver.c provides it.
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "tinynurbs/tinynurbs.h"
#include "task_support/check.h"

#include <cmath>
#include <stdexcept>
#include <vector>

using namespace tinynurbs;
using V = glm::dvec3;

namespace
{

RayQueryOptions tightOptions()
{
    RayQueryOptions opt;
    opt.position_tolerance = 1e-8;
    opt.parameter_tolerance = 1e-9;
    opt.box_budget = 100000;
    return opt;
}

// Bilinear folded patch S(u,v) = (u, v, uv) on the unit domain.
RationalSurface3d bilinear()
{
    RationalSurface3d s;
    s.degree_u = 1;
    s.degree_v = 1;
    s.knots_u = {0, 0, 1, 1};
    s.knots_v = {0, 0, 1, 1};
    s.control_points =
        array2<V>(2, 2, {V(0, 0, 0), V(0, 1, 0), V(1, 0, 0), V(1, 1, 1)});
    s.weights = array2<double>(2, 2, {1, 1, 1, 1});
    return s;
}

// Unit-radius rational quarter-cylinder band, height 2 along z.
RationalSurface3d quarterCylinder()
{
    const double w = std::sqrt(0.5);
    RationalSurface3d s;
    s.degree_u = 2;
    s.degree_v = 1;
    s.knots_u = {0, 0, 0, 1, 1, 1};
    s.knots_v = {0, 0, 1, 1};
    s.control_points = array2<V>(
        3, 2, {V(1, 0, 0), V(1, 0, 2), V(1, 1, 0), V(1, 1, 2), V(0, 1, 0), V(0, 1, 2)});
    s.weights = array2<double>(3, 2, {1, 1, w, w, 1, 1});
    return s;
}

// Flat degree-(1,1) patch with a non-unit domain, offset by (ox, oy).
RationalSurface3d plane(double ox, double oy)
{
    RationalSurface3d s;
    s.degree_u = 1;
    s.degree_v = 1;
    s.knots_u = {0, 0, 2, 2};
    s.knots_v = {0, 0, 3, 3};
    s.control_points = array2<V>(
        2, 2, {V(ox, oy, 0), V(ox, oy + 3, 0), V(ox + 2, oy, 0), V(ox + 2, oy + 3, 0)});
    s.weights = array2<double>(2, 2, {1, 1, 1, 1});
    return s;
}

// Degree-2 patch with three knot spans per axis and 5x5 control points,
// lying in the plane z = z0 with lower-left corner (ox, oy).
RationalSurface3d gridPatch(double ox, double oy, double z0)
{
    RationalSurface3d s;
    s.degree_u = 2;
    s.degree_v = 2;
    s.knots_u = {0, 0, 0, 1, 2, 3, 3, 3};
    s.knots_v = {0, 0, 0, 1, 2, 3, 3, 3};
    array2<V> cp(5, 5);
    array2<double> w(5, 5);
    for (int i = 0; i < 5; ++i)
        for (int j = 0; j < 5; ++j)
        {
            cp(i, j) = V(ox + i, oy + j, z0);
            w(i, j) = 1.0;
        }
    s.control_points = cp;
    s.weights = w;
    return s;
}

// Number of non-empty knot spans overlapped by basis function cp, derived
// directly from its knot support [knots[cp], knots[cp+degree+1]).
unsigned supportSpans(unsigned degree, const std::vector<double> &knots, unsigned cp)
{
    unsigned count = 0;
    int last = static_cast<int>(knots.size()) - static_cast<int>(degree) - 2;
    double lo = knots[cp];
    double hi = knots[cp + degree + 1];
    for (int k = static_cast<int>(degree); k <= last; ++k)
        if (knots[k] < knots[k + 1] && knots[k + 1] > lo && knots[k] < hi) ++count;
    return count;
}

V suCrossSv(const RationalSurface3d &s, double u, double v)
{
    array2<V> d = surfaceDerivatives(s, 1, u, v);
    return glm::normalize(glm::cross(d(1, 0), d(0, 1)));
}

// ---------------------------------------------------------------- roots ---

int roots_cylinder_analytic()
{
    RaySurfaceScene scene(std::vector<RationalSurface3d>{quarterCylinder()});
    auto r = scene.intersect(V(0, 0.5, 1), V(1, 0, 0), -1e9, 1e9, tightOptions());
    TASK_CHECK(r.error == RaySceneError::None);
    TASK_CHECK(r.complete);
    TASK_CHECK(r.hits.size() == 1);
    const auto &h = r.hits[0];
    TASK_CHECK(std::abs(h.t - std::sqrt(0.75)) < 1e-8);
    TASK_CHECK(std::abs(h.point.x - std::sqrt(0.75)) < 1e-8);
    TASK_CHECK(std::abs(h.point.y - 0.5) < 1e-8);
    TASK_CHECK(std::abs(h.point.z - 1.0) < 1e-8);
    // Radial normal of the unit cylinder equals the hit point in xy.
    TASK_CHECK(std::abs(h.normal.x - h.point.x) < 1e-10);
    TASK_CHECK(std::abs(h.normal.y - h.point.y) < 1e-10);
    TASK_CHECK(std::abs(h.normal.z) < 1e-10);
    TASK_CHECK(std::abs(glm::length(h.normal) - 1.0) < 1e-12);
    return 0;
}

int roots_bilinear_double_root()
{
    RaySurfaceScene scene(std::vector<RationalSurface3d>{bilinear()});
    auto r = scene.intersect(V(0, 0, -0.16), V(1, 1, 1), -1e9, 1e9, tightOptions());
    TASK_CHECK(r.error == RaySceneError::None);
    TASK_CHECK(r.complete);
    TASK_CHECK(r.hits.size() == 2);
    TASK_CHECK(r.hits[0].t < r.hits[1].t);
    TASK_CHECK(std::abs(r.hits[0].t - 0.2) < 1e-8);
    TASK_CHECK(std::abs(r.hits[1].t - 0.8) < 1e-8);
    for (const auto &h : r.hits)
    {
        // O + tD back-substitution residual bound.
        V residual = V(0, 0, -0.16) + h.t * V(1, 1, 1) - h.point;
        TASK_CHECK(glm::length(residual) <= 1e-8);
        TASK_CHECK(std::abs(glm::length(h.normal) - 1.0) < 1e-12);
    }
    return 0;
}

int roots_sorting_window_domain()
{
    std::vector<RationalSurface3d> patches;
    patches.push_back(plane(0, 0));
    RationalSurface3d upper = plane(0, 0);
    for (size_t i = 0; i < upper.control_points.size(); ++i) upper.control_points[i].z = 2;
    patches.push_back(upper);
    RaySurfaceScene scene(patches);

    auto r = scene.intersect(V(1, 1.5, -1), V(0, 0, 1), 0, 10, tightOptions());
    TASK_CHECK(r.complete);
    TASK_CHECK(r.hits.size() == 2);
    TASK_CHECK(r.hits[0].t <= r.hits[1].t);
    TASK_CHECK(r.hits[0].surface_index == 0);
    TASK_CHECK(r.hits[1].surface_index == 1);
    // Non-unit parameter domain: the hit maps to u=1, v=1.5.
    TASK_CHECK(std::abs(r.hits[0].u - 1.0) < 1e-9);
    TASK_CHECK(std::abs(r.hits[0].v - 1.5) < 1e-9);

    // Finite distance window keeps only the nearer surface.
    auto window = scene.intersect(V(1, 1.5, -1), V(0, 0, 1), 1.5, 10, tightOptions());
    TASK_CHECK(window.complete);
    TASK_CHECK(window.hits.size() == 1);
    TASK_CHECK(window.hits[0].surface_index == 1);
    // Half-open upper bound: a ray ending exactly at the surface misses it.
    auto edge = scene.intersect(V(1, 1.5, -1), V(0, 0, 1), 0, 1.0, tightOptions());
    TASK_CHECK(edge.complete);
    TASK_CHECK(edge.hits.empty());
    return 0;
}

int roots_same_point_distinct_uv()
{
    std::vector<RationalSurface3d> patches;
    patches.push_back(plane(0, 0));
    RationalSurface3d reparam = plane(0, 0);
    reparam.knots_u = {0, 0, 5, 5};
    reparam.knots_v = {0, 0, 7, 7};
    reparam.control_points =
        array2<V>(2, 2, {V(0, 0, 0), V(0, 3, 0), V(2, 0, 0), V(2, 3, 0)});
    patches.push_back(reparam);
    RaySurfaceScene scene(patches);

    auto r = scene.intersect(V(1, 1.5, -1), V(0, 0, 1), 0, 10, tightOptions());
    TASK_CHECK(r.hits.size() == 2); // distinct UV roots are both retained
    TASK_CHECK(glm::length(r.hits[0].point - r.hits[1].point) < 1e-10);
    double uv_gap = std::abs(r.hits[0].u - r.hits[1].u) +
                    std::abs(r.hits[0].v - r.hits[1].v);
    TASK_CHECK(uv_gap > 1e-6);
    return 0;
}

int roots_knot_normal_and_merge()
{
    RationalSurface3d s;
    s.degree_u = 1;
    s.degree_v = 1;
    s.knots_u = {0, 0, 0.5, 1, 1};
    s.knots_v = {0, 0, 1, 1};
    s.control_points = array2<V>(
        3, 2, {V(0, 0, 0), V(0, 1, 0), V(0.5, 0, 0), V(0.5, 1, 0), V(1, 0, 0), V(1, 1, 0)});
    s.weights = array2<double>(3, 2, std::vector<double>(6, 1.0));
    RaySurfaceScene scene(std::vector<RationalSurface3d>{s});

    // Root on the interior knot line: discovered from both elements, merged.
    auto r = scene.intersect(V(0.5, 0.5, -1), V(0, 0, 1), 0, 10, tightOptions());
    TASK_CHECK(r.complete);
    TASK_CHECK(r.hits.size() == 1);
    TASK_CHECK(std::abs(r.hits[0].u - 0.5) < 1e-12);
    // Normal uses the right-hand element and follows Su x Sv.
    V expected = suCrossSv(s, 0.5000001, 0.5);
    TASK_CHECK(glm::length(r.hits[0].normal - expected) < 1e-12);

    // Upper domain bound resolves through the last (left) element.
    auto top = scene.intersect(V(1.0, 0.5, -1), V(0, 0, 1), 0, 10, tightOptions());
    TASK_CHECK(top.complete);
    TASK_CHECK(top.hits.size() == 1);
    TASK_CHECK(std::abs(top.hits[0].u - 1.0) < 1e-12);
    return 0;
}

int roots_budget_and_validation()
{
    std::vector<RationalSurface3d> patches;
    for (int k = 0; k < 4; ++k) patches.push_back(gridPatch(1000.0 * k, 1000.0 * k, 0));
    RaySurfaceScene scene(patches);

    RayQueryOptions starved = tightOptions();
    starved.box_budget = 2;
    auto r = scene.intersect(V(1, 1, -1), V(0, 0, 1), 0, 10, starved);
    TASK_CHECK(r.error == RaySceneError::None);
    TASK_CHECK(!r.complete);
    TASK_CHECK(!r.unresolved.empty());
    TASK_CHECK(r.box_visits > 0);
    for (const auto &u : r.unresolved)
    {
        // Envelopes must be genuine parameter ranges, never zero-width.
        TASK_CHECK(u.u_max > u.u_min);
        TASK_CHECK(u.v_max > u.v_min);
    }

    // Query-time validation rejects before any ray work.
    TASK_CHECK(scene.intersect(V(0), V(0), 0, 1, tightOptions()).error ==
               RaySceneError::ZeroDirection);
    TASK_CHECK(scene.intersect(V(0), V(1, 0, 0), 2, 1, tightOptions()).error ==
               RaySceneError::InvalidInterval);
    RayQueryOptions bad_tol = tightOptions();
    bad_tol.position_tolerance = 0;
    TASK_CHECK(scene.intersect(V(0), V(1, 0, 0), 0, 1, bad_tol).error ==
               RaySceneError::NonPositiveTolerance);
    RayQueryOptions bad_budget = tightOptions();
    bad_budget.box_budget = 0;
    TASK_CHECK(scene.intersect(V(0), V(1, 0, 0), 0, 1, bad_budget).error ==
               RaySceneError::ZeroBudget);
    TASK_CHECK(scene.intersect(V(std::nan(""), 0, 0), V(1, 0, 0), 0, 1, tightOptions())
                   .error == RaySceneError::NonFiniteInput);
    return 0;
}

int roots_tangent_coincident()
{
    RaySurfaceScene scene(std::vector<RationalSurface3d>{plane(0, 0)});
    // Ray lying entirely inside the plane: infinite coincidence must be
    // reported as unresolved, never silently as a clean miss.
    auto r = scene.intersect(V(0.5, 1, 0), V(1, 0, 0), 0, 1.0, tightOptions());
    TASK_CHECK(!r.complete);
    TASK_CHECK(!r.unresolved.empty());
    for (const auto &u : r.unresolved)
    {
        TASK_CHECK(u.surface_index == 0);
        TASK_CHECK(u.u_max >= u.u_min);
        TASK_CHECK(u.v_max >= u.v_min);
    }
    return 0;
}

// --------------------------------------------------------------- updates ---

int updates_sparse_edit_requery()
{
    std::vector<RationalSurface3d> patches;
    for (int k = 0; k < 4; ++k) patches.push_back(gridPatch(1000.0 * k, 1000.0 * k, 0));
    RaySurfaceScene scene(patches);
    RaySceneStats before = scene.stats();

    unsigned expect_u = supportSpans(2, patches[0].knots_u, 1);
    unsigned expect_v = supportSpans(2, patches[0].knots_v, 1);
    auto er = scene.updateControlPoints(
        std::vector<ControlPointEdit>{ControlPointEdit{0, 1, 1, V(1, 1, 0.5), 2.0}});
    TASK_CHECK(er.error == RaySceneEditError::None);
    TASK_CHECK(er.reextracted_elements == expect_u * expect_v);
    TASK_CHECK(er.refit_nodes > 0);

    RaySceneStats after = scene.stats();
    TASK_CHECK(after.element_extractions == before.element_extractions + expect_u * expect_v);
    TASK_CHECK(after.total_tree_nodes == before.total_tree_nodes); // topology kept
    TASK_CHECK(after.total_elements == before.total_elements);

    RationalSurface3d reference = patches[0];
    reference.control_points(1, 1) = V(1, 1, 0.5);
    reference.weights(1, 1) = 2.0;
    auto r = scene.intersect(V(1, 1, -1), V(0, 0, 1), 0, 10, tightOptions());
    TASK_CHECK(r.complete);
    TASK_CHECK(r.hits.size() == 1);
    V truth = surfacePoint(reference, r.hits[0].u, r.hits[0].v);
    TASK_CHECK(glm::length(r.hits[0].point - truth) < 1e-10);
    return 0;
}

int updates_reject_then_retry()
{
    std::vector<RationalSurface3d> patches;
    for (int k = 0; k < 4; ++k) patches.push_back(gridPatch(1000.0 * k, 1000.0 * k, 0));
    RaySurfaceScene scene(patches);
    RaySceneStats before = scene.stats();

    ControlPointEdit good{0, 1, 1, V(1, 1, 0.5), 2.0};
    TASK_CHECK(scene.updateControlPoints(std::vector<ControlPointEdit>{good, good}).error ==
               RaySceneEditError::DuplicateAddress);
    TASK_CHECK(scene.updateControlPoints(
                   std::vector<ControlPointEdit>{ControlPointEdit{0, 0, 1, V(0), 0.0}})
                   .error == RaySceneEditError::NonPositiveWeight);
    TASK_CHECK(scene.updateControlPoints(
                   std::vector<ControlPointEdit>{ControlPointEdit{99, 0, 0, V(0), 1.0}})
                   .error == RaySceneEditError::SurfaceIndexOutOfRange);
    TASK_CHECK(scene.updateControlPoints(
                   std::vector<ControlPointEdit>{ControlPointEdit{0, 7, 0, V(0), 1.0}})
                   .error == RaySceneEditError::ControlPointIndexOutOfRange);
    TASK_CHECK(scene
                   .updateControlPoints(std::vector<ControlPointEdit>{
                       ControlPointEdit{0, 0, 1, V(std::nan(""), 0, 0), 1.0}})
                   .error == RaySceneEditError::NonFiniteValue);

    // A rejected batch changes nothing: no extractions, queries still work.
    TASK_CHECK(scene.stats().element_extractions == before.element_extractions);
    auto still = scene.intersect(V(1, 1, -1), V(0, 0, 1), 0, 10, tightOptions());
    TASK_CHECK(still.complete);
    TASK_CHECK(still.hits.size() == 1);
    TASK_CHECK(std::abs(still.hits[0].t - 1.0) < 1e-8);

    // Retry with a legal batch succeeds.
    TASK_CHECK(scene.updateControlPoints(std::vector<ControlPointEdit>{good}).error ==
               RaySceneEditError::None);
    return 0;
}

int updates_empty_absolute_and_lifetime()
{
    std::vector<RationalSurface3d> patches;
    for (int k = 0; k < 4; ++k) patches.push_back(gridPatch(1000.0 * k, 1000.0 * k, 0));
    RaySurfaceScene scene(patches);
    RaySceneStats before = scene.stats();

    auto empty = scene.updateControlPoints(std::vector<ControlPointEdit>{});
    TASK_CHECK(empty.error == RaySceneEditError::None);
    TASK_CHECK(empty.reextracted_elements == 0);
    TASK_CHECK(empty.refit_nodes == 0);
    TASK_CHECK(scene.stats().element_extractions == before.element_extractions);

    // Repeated absolute edits: the second value replaces the first outright.
    scene.updateControlPoints(
        std::vector<ControlPointEdit>{ControlPointEdit{0, 0, 0, V(0, 0, 0.7), 1.0}});
    scene.updateControlPoints(
        std::vector<ControlPointEdit>{ControlPointEdit{0, 0, 0, V(0, 0, 0.3), 1.0}});
    RationalSurface3d reference = patches[0];
    reference.control_points(0, 0) = V(0, 0, 0.3);
    auto r = scene.intersect(V(0.1, 0.1, -1), V(0, 0, 1), 0, 10, tightOptions());
    TASK_CHECK(r.complete && r.hits.size() == 1);
    V truth = surfacePoint(reference, r.hits[0].u, r.hits[0].v);
    TASK_CHECK(glm::length(r.hits[0].point - truth) < 1e-10);

    // Releasing / mutating the input storage must not affect the scene.
    std::vector<ControlPointEdit> batch{ControlPointEdit{1, 0, 0, V(1000, 1000, 5), 1}};
    TASK_CHECK(scene.updateControlPoints(batch).error == RaySceneEditError::None);
    batch.clear();
    batch.shrink_to_fit();
    auto later = scene.intersect(V(1000.2, 1000.2, 0), V(0, 0, 1), 0, 10, tightOptions());
    TASK_CHECK(later.complete && later.hits.size() == 1);

    // Old query results own their storage and survive later queries/edits.
    RayQueryResult saved = r;
    scene.updateControlPoints(
        std::vector<ControlPointEdit>{ControlPointEdit{0, 0, 0, V(0, 0, 0.9), 1.0}});
    TASK_CHECK(saved.hits.size() == 1);
    TASK_CHECK(std::abs(saved.hits[0].point.z - r.hits[0].point.z) < 1e-15);

    // The scene owns its surfaces; destroying the source vector is safe.
    std::vector<RationalSurface3d> source{bilinear()};
    RaySurfaceScene owned(source);
    source.clear();
    auto q = owned.intersect(V(0, 0, -0.16), V(1, 1, 1), -1e9, 1e9, tightOptions());
    TASK_CHECK(q.complete && q.hits.size() == 2);
    return 0;
}

// ---------------------------------------------------------------- bounds ---

int bounds_hierarchical_pruning()
{
    std::vector<RationalSurface3d> patches;
    for (int k = 0; k < 4; ++k) patches.push_back(gridPatch(1000.0 * k, 1000.0 * k, 0));
    RaySurfaceScene scene(patches);
    RaySceneStats st = scene.stats();
    TASK_CHECK(st.surface_count == 4);
    TASK_CHECK(st.total_elements == 36);
    TASK_CHECK(st.total_tree_nodes > st.total_elements);

    // A ray missing every patch only opens the four surface root boxes; no
    // leaf patch is ever extracted for subdivision.
    auto miss = scene.intersect(V(-5, -5, -1), V(0, 0, 1), 0, 10, tightOptions());
    TASK_CHECK(miss.complete);
    TASK_CHECK(miss.hits.empty());
    TASK_CHECK(miss.box_visits == 4);
    TASK_CHECK(miss.patch_boxes_tested == 0);

    // A hit on one far patch visits only a fraction of the tree nodes.
    auto hit = scene.intersect(V(1, 1, -1), V(0, 0, 1), 0, 10, tightOptions());
    TASK_CHECK(hit.complete);
    TASK_CHECK(hit.hits.size() == 1);
    std::size_t node_visits = hit.box_visits - hit.patch_boxes_tested;
    TASK_CHECK(node_visits < st.total_tree_nodes);
    TASK_CHECK(node_visits > 4);
    // Counters are real per-query numbers, not constants.
    TASK_CHECK(hit.box_visits != miss.box_visits);
    TASK_CHECK(hit.patch_boxes_tested > 0);
    return 0;
}

int bounds_extraction_and_refit_counts()
{
    std::vector<RationalSurface3d> patches;
    for (int k = 0; k < 4; ++k) patches.push_back(gridPatch(1000.0 * k, 1000.0 * k, 0));
    RaySurfaceScene scene(patches);
    RaySceneStats before = scene.stats();
    TASK_CHECK(before.element_extractions == before.total_elements);

    // Corner control point of surface 3: only its own basis support is
    // re-extracted; the other three surfaces are never touched.
    unsigned cu = supportSpans(2, patches[3].knots_u, 3);
    unsigned cv = supportSpans(2, patches[3].knots_v, 0);
    auto er = scene.updateControlPoints(
        std::vector<ControlPointEdit>{ControlPointEdit{3, 3, 0, V(3003, 3000, 0), 1.0}});
    TASK_CHECK(er.error == RaySceneEditError::None);
    TASK_CHECK(er.reextracted_elements == cu * cv);
    TASK_CHECK(er.reextracted_elements < before.total_elements);

    RaySceneStats after = scene.stats();
    TASK_CHECK(after.element_extractions == before.element_extractions + cu * cv);
    // Hierarchy is refit in place: connectivity and leaf count are unchanged.
    TASK_CHECK(after.total_tree_nodes == before.total_tree_nodes);
    TASK_CHECK(after.total_elements == before.total_elements);
    TASK_CHECK(after.surface_count == before.surface_count);
    return 0;
}

int bounds_unsupported_surface_untouched()
{
    std::vector<RationalSurface3d> patches;
    for (int k = 0; k < 4; ++k) patches.push_back(gridPatch(1000.0 * k, 1000.0 * k, 0));
    RaySurfaceScene scene(patches);

    // Edit a control point on surface 2 only.
    unsigned cu = supportSpans(2, patches[2].knots_u, 0);
    unsigned cv = supportSpans(2, patches[2].knots_v, 0);
    auto er = scene.updateControlPoints(
        std::vector<ControlPointEdit>{ControlPointEdit{2, 0, 0, V(2000, 2000, 0.25), 1.0}});
    TASK_CHECK(er.error == RaySceneEditError::None);
    // All re-extractions belong to surface 2: total equals its local support.
    TASK_CHECK(er.reextracted_elements == cu * cv);

    // Surface 0 geometry is unchanged and still traces exactly.
    auto r = scene.intersect(V(1, 1, -1), V(0, 0, 1), 0, 10, tightOptions());
    TASK_CHECK(r.complete);
    TASK_CHECK(r.hits.size() == 1);
    TASK_CHECK(r.hits[0].surface_index == 0);
    TASK_CHECK(std::abs(r.hits[0].t - 1.0) < 1e-8);
    return 0;
}

int roots_unsupported_input_rejected()
{
    bool threw = false;
    // Degree outside the supported 1..3 range.
    RationalSurface3d bad = quarterCylinder();
    bad.degree_u = 4;
    try
    {
        RaySurfaceScene scene(std::vector<RationalSurface3d>{bad});
    }
    catch (const std::invalid_argument &)
    {
        threw = true;
    }
    TASK_CHECK(threw);

    // Non-positive weight.
    threw = false;
    RationalSurface3d bad_w = bilinear();
    bad_w.weights(0, 0) = -1.0;
    try
    {
        RaySurfaceScene scene(std::vector<RationalSurface3d>{bad_w});
    }
    catch (const std::invalid_argument &)
    {
        threw = true;
    }
    TASK_CHECK(threw);

    // Interior knot multiplicity above the degree is rejected.
    threw = false;
    RationalSurface3d bad_k = bilinear();
    bad_k.knots_u = {0, 0, 0.5, 0.5, 1, 1}; // multiplicity 2 > degree 1
    bad_k.control_points = array2<V>(
        3, 2, {V(0), V(0, 1, 0), V(0.5, 0, 0), V(0.5, 1, 0), V(1, 0, 0), V(1, 1, 0)});
    bad_k.weights = array2<double>(3, 2, std::vector<double>(6, 1.0));
    try
    {
        RaySurfaceScene scene(std::vector<RationalSurface3d>{bad_k});
    }
    catch (const std::invalid_argument &)
    {
        threw = true;
    }
    TASK_CHECK(threw);
    return 0;
}

} // namespace

const struct task_case task_cases[] = {
    {"roots", "cylinder_analytic", roots_cylinder_analytic},
    {"roots", "bilinear_double_root", roots_bilinear_double_root},
    {"roots", "sorting_window_domain", roots_sorting_window_domain},
    {"roots", "same_point_distinct_uv", roots_same_point_distinct_uv},
    {"roots", "knot_normal_and_merge", roots_knot_normal_and_merge},
    {"roots", "budget_and_validation", roots_budget_and_validation},
    {"roots", "tangent_coincident", roots_tangent_coincident},
    {"roots", "unsupported_input_rejected", roots_unsupported_input_rejected},

    {"updates", "sparse_edit_requery", updates_sparse_edit_requery},
    {"updates", "reject_then_retry", updates_reject_then_retry},
    {"updates", "empty_absolute_lifetime", updates_empty_absolute_and_lifetime},

    {"bounds", "hierarchical_pruning", bounds_hierarchical_pruning},
    {"bounds", "extraction_refit_counts", bounds_extraction_and_refit_counts},
    {"bounds", "unsupported_surface_untouched", bounds_unsupported_surface_untouched},
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
