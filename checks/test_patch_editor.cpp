/**
 * Checks for the PatchEditor and patch mesh generation.
 *
 * Three groups are registered: continuity, projection and lifecycle.  This
 * translation unit deliberately does not define main(); task_support/driver.c
 * provides it.
 *
 * Use of this source code is governed by a BSD-style license that can be found in
 * the LICENSE file.
 */

#include "../task_support/check.h"

#include <tinynurbs/tinynurbs.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <functional>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <utility>
#include <vector>

using tinynurbs::PatchEditor;
using tinynurbs::PatchEditorSnapshot;
using tinynurbs::PatchHandle;
using tinynurbs::PatchMesh;
using tinynurbs::PatchSeam;
using tinynurbs::RationalSurface3d;
using tinynurbs::SurfaceEdge;

namespace
{

using dvec3 = glm::dvec3;

constexpr double kTol = 1e-8;

/////////////////////////////////////////////////////////////////////
// Geometry construction helpers.
/////////////////////////////////////////////////////////////////////

std::vector<double> clampedKnots(unsigned degree, double a, double b,
                                 const std::vector<double> &interior)
{
    std::vector<double> knots;
    for (unsigned i = 0; i <= degree; ++i)
    {
        knots.push_back(a);
    }
    for (double v : interior)
    {
        knots.push_back(v);
    }
    for (unsigned i = 0; i <= degree; ++i)
    {
        knots.push_back(b);
    }
    return knots;
}

RationalSurface3d makeSurface(unsigned pu, unsigned pv, const std::vector<double> &ku,
                              const std::vector<double> &kv, int seed, const dvec3 &offset,
                              double zscale)
{
    RationalSurface3d srf;
    srf.degree_u = pu;
    srf.degree_v = pv;
    srf.knots_u = ku;
    srf.knots_v = kv;
    const int nu = static_cast<int>(ku.size()) - static_cast<int>(pu) - 1;
    const int nv = static_cast<int>(kv.size()) - static_cast<int>(pv) - 1;
    srf.control_points.resize(static_cast<std::size_t>(nu), static_cast<std::size_t>(nv));
    srf.weights.resize(static_cast<std::size_t>(nu), static_cast<std::size_t>(nv));
    unsigned state = static_cast<unsigned>(seed) * 2654435761u + 1u;
    auto next = [&state]() {
        state = state * 1664525u + 1013904223u;
        return static_cast<double>(state >> 8) / 16777216.0; // [0,1)
    };
    for (int i = 0; i < nu; ++i)
    {
        for (int j = 0; j < nv; ++j)
        {
            const double x = 0.4 * i + 0.25 * (next() - 0.5);
            const double y = 0.4 * j + 0.25 * (next() - 0.5);
            const double z = zscale * (0.3 * std::sin(0.9 * i + 0.6 * j) + 0.1 * next());
            srf.control_points(static_cast<std::size_t>(i), static_cast<std::size_t>(j)) =
                offset + dvec3(x, y, z);
            srf.weights(static_cast<std::size_t>(i), static_cast<std::size_t>(j)) =
                0.45 + 2.1 * next();
        }
    }
    return srf;
}

std::pair<double, double> domainOf(const std::vector<double> &knots, unsigned degree)
{
    return {knots[degree], knots[knots.size() - degree - 1]};
}

// Maximum position and cross-derivative seam residual over a dense sampling.
// DA / DB are the derivatives with respect to each patch's normalized
// transverse parameter measured from the boundary into the patch interior.
void seamResidual(const RationalSurface3d &A, SurfaceEdge ea, const RationalSurface3d &B,
                  SurfaceEdge eb, bool reversed, int samples, double &maxPos, double &maxDer)
{
    maxPos = 0.0;
    maxDer = 0.0;
    const auto au = domainOf(A.knots_u, A.degree_u);
    const auto av = domainOf(A.knots_v, A.degree_v);
    const auto bu = domainOf(B.knots_u, B.degree_u);
    const auto bv = domainOf(B.knots_v, B.degree_v);
    const bool aAlongU = (ea == SurfaceEdge::VMin || ea == SurfaceEdge::VMax);
    const bool bAlongU = (eb == SurfaceEdge::VMin || eb == SurfaceEdge::VMax);

    for (int k = 0; k <= samples; ++k)
    {
        const double t = static_cast<double>(k) / static_cast<double>(samples);
        const double tb = reversed ? 1.0 - t : t;
        const double ua = au.first + t * (au.second - au.first);
        const double va = av.first + t * (av.second - av.first);
        const double ub = bu.first + tb * (bu.second - bu.first);
        const double vb = bv.first + tb * (bv.second - bv.first);

        dvec3 pa, pb, dA, dB;
        if (aAlongU)
        {
            const bool atMin = ea == SurfaceEdge::VMin;
            const double s = atMin ? av.first : av.second;
            pa = tinynurbs::surfacePoint(A, ua, s);
            dA = tinynurbs::surfaceDerivatives(A, 1, ua, s)(0, 1) *
                 ((av.second - av.first) * (atMin ? 1.0 : -1.0));
        }
        else
        {
            const bool atMin = ea == SurfaceEdge::UMin;
            const double s = atMin ? au.first : au.second;
            pa = tinynurbs::surfacePoint(A, s, va);
            dA = tinynurbs::surfaceDerivatives(A, 1, s, va)(1, 0) *
                 ((au.second - au.first) * (atMin ? 1.0 : -1.0));
        }
        if (bAlongU)
        {
            const bool atMin = eb == SurfaceEdge::VMin;
            const double s = atMin ? bv.first : bv.second;
            pb = tinynurbs::surfacePoint(B, ub, s);
            dB = tinynurbs::surfaceDerivatives(B, 1, ub, s)(0, 1) *
                 ((bv.second - bv.first) * (atMin ? 1.0 : -1.0));
        }
        else
        {
            const bool atMin = eb == SurfaceEdge::UMin;
            const double s = atMin ? bu.first : bu.second;
            pb = tinynurbs::surfacePoint(B, s, vb);
            dB = tinynurbs::surfaceDerivatives(B, 1, s, vb)(1, 0) *
                 ((bu.second - bu.first) * (atMin ? 1.0 : -1.0));
        }
        maxPos = std::max(maxPos, static_cast<double>(glm::length(pa - pb)));
        maxDer = std::max(maxDer, static_cast<double>(glm::length(dA + dB)));
    }
}

double controlPointDistance(const RationalSurface3d &A, const RationalSurface3d &B)
{
    if (A.control_points.rows() != B.control_points.rows() ||
        A.control_points.cols() != B.control_points.cols())
    {
        return 1e300;
    }
    double d = 0.0;
    for (std::size_t i = 0; i < A.control_points.rows(); ++i)
    {
        for (std::size_t j = 0; j < A.control_points.cols(); ++j)
        {
            d = std::max(d, static_cast<double>(glm::length(
                                A.control_points(i, j) - B.control_points(i, j))));
        }
    }
    return d;
}

// Two patches, same degree along the seam, different along/transverse knot
// vectors and different non-constant weights.
std::vector<RationalSurface3d> makePair(unsigned p, double seamDomainA0, double seamDomainA1,
                                        bool verticalSeam, double off)
{
    // Patch A: seam is VMax if verticalSeam (along u), else UMax (along v).
    std::vector<double> kaU, kaV, kbU, kbV;
    if (verticalSeam)
    {
        kaU = clampedKnots(p, -3.0, 5.0, p >= 2 ? std::vector<double>{0.7} : std::vector<double>{});
        kaV = clampedKnots(p, 10.0, 12.0, p >= 2 ? std::vector<double>{11.1} : std::vector<double>{});
        kbU = clampedKnots(p, 20.0, 23.0, p >= 2 ? std::vector<double>{21.4} : std::vector<double>{});
        kbV = clampedKnots(p, -2.0, 4.0, p >= 2 ? std::vector<double>{1.3} : std::vector<double>{});
    }
    else
    {
        kaU = clampedKnots(p, 10.0, 12.0, p >= 2 ? std::vector<double>{11.1} : std::vector<double>{});
        kaV = clampedKnots(p, -3.0, 5.0, p >= 2 ? std::vector<double>{0.7} : std::vector<double>{});
        kbU = clampedKnots(p, -2.0, 4.0, p >= 2 ? std::vector<double>{1.3} : std::vector<double>{});
        kbV = clampedKnots(p, 20.0, 23.0, p >= 2 ? std::vector<double>{21.4} : std::vector<double>{});
    }
    (void)seamDomainA0;
    (void)seamDomainA1;
    std::vector<RationalSurface3d> out;
    out.push_back(makeSurface(p, p, kaU, kaV, 11, dvec3(0, 0, 0), 0.5));
    out.push_back(makeSurface(p, p, kbU, kbV, 29, dvec3(off, 0, 0), 0.7));
    return out;
}

PatchSeam verticalSeam() { return {0, SurfaceEdge::VMax, 1, SurfaceEdge::VMin, false}; }
PatchSeam horizontalSeam() { return {0, SurfaceEdge::UMax, 1, SurfaceEdge::UMin, false}; }

// A chain of three degree-2 patches obtained by splitting one smooth surface.
std::array<RationalSurface3d, 3> makeChain()
{
    RationalSurface3d whole;
    whole.degree_u = 2;
    whole.degree_v = 2;
    whole.knots_u = {0, 0, 0, 1, 2, 3, 3, 3};
    whole.knots_v = {0, 0, 0, 1, 1, 1};
    whole.control_points.resize(5, 3);
    whole.weights.resize(5, 3);
    for (int i = 0; i < 5; ++i)
    {
        for (int j = 0; j < 3; ++j)
        {
            whole.control_points(static_cast<std::size_t>(i), static_cast<std::size_t>(j)) =
                dvec3(0.7 * i, 0.7 * j, 0.25 * i * j + 0.1 * i * i);
            whole.weights(static_cast<std::size_t>(i), static_cast<std::size_t>(j)) =
                0.6 + 0.2 * ((i + 2 * j) % 4);
        }
    }
    auto first = tinynurbs::surfaceSplitU(whole, 1.0);
    auto second = tinynurbs::surfaceSplitU(std::get<1>(first), 2.0);
    return {std::get<0>(first), std::get<0>(second), std::get<1>(second)};
}

// A 2x2 arrangement of four patches around a shared corner.
std::array<RationalSurface3d, 4> makeFourAroundCorner()
{
    RationalSurface3d whole;
    whole.degree_u = 2;
    whole.degree_v = 2;
    whole.knots_u = {0, 0, 0, 1, 2, 2, 2};
    whole.knots_v = {0, 0, 0, 1, 2, 2, 2};
    whole.control_points.resize(4, 4);
    whole.weights.resize(4, 4);
    for (int i = 0; i < 4; ++i)
    {
        for (int j = 0; j < 4; ++j)
        {
            whole.control_points(static_cast<std::size_t>(i), static_cast<std::size_t>(j)) =
                dvec3(0.6 * i, 0.6 * j, 0.2 * std::sin(0.8 * i + 0.5 * j));
            whole.weights(static_cast<std::size_t>(i), static_cast<std::size_t>(j)) =
                0.7 + 0.3 * ((i + j) % 3);
        }
    }
    auto cols = tinynurbs::surfaceSplitU(whole, 1.0);
    auto s00 = tinynurbs::surfaceSplitV(std::get<0>(cols), 1.0);
    auto s10 = tinynurbs::surfaceSplitV(std::get<1>(cols), 1.0);
    return {std::get<0>(s00), std::get<1>(s00), std::get<0>(s10), std::get<1>(s10)};
}

/////////////////////////////////////////////////////////////////////
// case: continuity
/////////////////////////////////////////////////////////////////////

int case_degrees_domains_reversal()
{
    for (unsigned p = 1; p <= 3; ++p)
    {
        for (int vertical = 1; vertical >= 0; --vertical)
        {
            for (int reversed = 0; reversed <= 1; ++reversed)
            {
                std::vector<RationalSurface3d> patches = makePair(p, 0, 1, vertical != 0,
                                                                  1.5 * vertical);
                PatchSeam seam = vertical ? verticalSeam() : horizontalSeam();
                seam.reversed = reversed != 0;
                PatchEditor editor(patches, {seam});
                PatchEditorSnapshot snap = editor.solve();
                double pos = 0.0, der = 0.0;
                seamResidual(snap.patches[0], seam.edge_a, snap.patches[1], seam.edge_b,
                             seam.reversed, 64, pos, der);
                TASK_CHECK(pos < kTol);
                TASK_CHECK(der < kTol);
            }
        }
    }
    return 0;
}

int case_cross_axis()
{
    // A's UMax edge (along v) meets B's VMin edge (along u): different axis.
    RationalSurface3d A = makeSurface(2, 2, clampedKnots(2, 0, 2, {0.6}),
                                      clampedKnots(2, 5, 8, {6.2}), 3, dvec3(0, 0, 0), 0.4);
    RationalSurface3d B = makeSurface(2, 2, clampedKnots(2, -4, -1, {-2.5}),
                                      clampedKnots(2, 10, 13, {11.0}), 4, dvec3(1, 1, 0), 0.6);
    PatchSeam seam{0, SurfaceEdge::UMax, 1, SurfaceEdge::VMin, true};
    PatchEditor editor({A, B}, {seam});
    PatchEditorSnapshot snap = editor.solve();
    double pos = 0.0, der = 0.0;
    seamResidual(snap.patches[0], seam.edge_a, snap.patches[1], seam.edge_b, true, 80, pos, der);
    TASK_CHECK(pos < kTol);
    TASK_CHECK(der < kTol);

    // Reverse direction as well.
    PatchSeam seam2{0, SurfaceEdge::UMax, 1, SurfaceEdge::VMin, false};
    PatchEditor editor2({A, B}, {seam2});
    PatchEditorSnapshot snap2 = editor2.solve();
    seamResidual(snap2.patches[0], seam2.edge_a, snap2.patches[1], seam2.edge_b, false, 80, pos,
                 der);
    TASK_CHECK(pos < kTol);
    TASK_CHECK(der < kTol);
    return 0;
}

int case_four_around_corner()
{
    std::array<RationalSurface3d, 4> p = makeFourAroundCorner();
    std::vector<RationalSurface3d> patches(p.begin(), p.end());
    std::vector<PatchSeam> seams = {
        {0, SurfaceEdge::UMax, 2, SurfaceEdge::UMin, false},
        {1, SurfaceEdge::UMax, 3, SurfaceEdge::UMin, false},
        {0, SurfaceEdge::VMax, 1, SurfaceEdge::VMin, false},
        {2, SurfaceEdge::VMax, 3, SurfaceEdge::VMin, false},
    };
    PatchEditor editor(patches, seams);
    PatchEditorSnapshot snap = editor.solve();
    for (const PatchSeam &s : seams)
    {
        double pos = 0.0, der = 0.0;
        seamResidual(snap.patches[static_cast<std::size_t>(s.patch_a)], s.edge_a,
                     snap.patches[static_cast<std::size_t>(s.patch_b)], s.edge_b, s.reversed, 48,
                     pos, der);
        TASK_CHECK(pos < kTol);
        TASK_CHECK(der < kTol);
    }

    // Drag a handle on one of the outer patches; all four seams stay exact.
    PatchHandle h;
    h.id = 7;
    h.patch = 3;
    h.u = 1.85;
    h.v = 1.85;
    h.target = tinynurbs::surfacePoint(snap.patches[3], h.u, h.v) + dvec3(0.5, -0.4, 0.9);
    editor.set_handles({h});
    PatchEditorSnapshot moved = editor.solve();
    for (const PatchSeam &s : seams)
    {
        double pos = 0.0, der = 0.0;
        seamResidual(moved.patches[static_cast<std::size_t>(s.patch_a)], s.edge_a,
                     moved.patches[static_cast<std::size_t>(s.patch_b)], s.edge_b, s.reversed, 48,
                     pos, der);
        TASK_CHECK(pos < kTol);
        TASK_CHECK(der < kTol);
    }
    TASK_CHECK(glm::length(tinynurbs::surfacePoint(moved.patches[3], h.u, h.v) - h.target) < kTol);
    return 0;
}

int case_samples_only_match()
{
    // Degree 2 along the seam; force the boundary of B to agree with A at
    // t = 0, 0.5 and 1 while the interiors of the curves differ.
    RationalSurface3d A = makeSurface(2, 2, clampedKnots(2, -1, 1, {}), clampedKnots(2, 0, 2, {}),
                                      5, dvec3(0, 0, 0), 0.3);
    RationalSurface3d B = makeSurface(2, 2, clampedKnots(2, 3, 5, {}), clampedKnots(2, 4, 6, {}),
                                      6, dvec3(0.2, 0, 0), 0.4);
    // The seam is A's VMax (along u) against B's VMin (along u), not reversed,
    // with both domains [a,a+2] and [b,b+2]: normalized t maps linearly.
    const int rows = static_cast<int>(A.control_points.rows());
    TASK_CHECK(static_cast<int>(B.control_points.rows()) == rows);
    for (int i = 0; i < rows; ++i)
    {
        const double t = static_cast<double>(i) / (rows - 1);
        for (int c = 0; c < 3; ++c)
        {
            B.control_points(static_cast<std::size_t>(i), 0)[c] =
                A.control_points(static_cast<std::size_t>(i), 2)[c];
        }
        (void)t;
    }
    // Now make the middle boundary control point of B such that the two
    // rational curves agree at the midpoint.  Basis values of a clamped
    // degree-2 curve over three control points at t=0.5 are (0.25, 0.5, 0.25).
    for (int c = 0; c < 3; ++c)
    {
        const double w0 = B.weights(0, 0);
        const double w1 = B.weights(1, 0);
        const double w2 = B.weights(2, 0);
        // Target: value of A's boundary curve at the midpoint of its middle
        // span.
        const auto domA = domainOf(A.knots_u, A.degree_u);
        const dvec3 target = tinynurbs::surfacePoint(A, 0.5 * (domA.first + domA.second),
                                                     domainOf(A.knots_v, A.degree_v).second);
        const double denom = 0.25 * w0 + 0.5 * w1 + 0.25 * w2;
        const double known = 0.25 * w0 * B.control_points(0, 0)[c] +
                             0.25 * w2 * B.control_points(2, 0)[c];
        B.control_points(1, 0)[c] = (target[c] * denom - known) / (0.5 * w1);
    }

    PatchSeam seam{0, SurfaceEdge::VMax, 1, SurfaceEdge::VMin, false};
    double pos0 = 0.0, der0 = 0.0;
    seamResidual(A, seam.edge_a, B, seam.edge_b, false, 97, pos0, der0);
    TASK_CHECK(pos0 > 1e-4); // The whole edge does not agree at rest.

    PatchEditor editor({A, B}, {seam});
    PatchEditorSnapshot snap = editor.solve();
    double pos = 0.0, der = 0.0;
    seamResidual(snap.patches[0], seam.edge_a, snap.patches[1], seam.edge_b, false, 97, pos, der);
    TASK_CHECK(pos < kTol);
    TASK_CHECK(der < kTol);
    return 0;
}

int case_refinement_preserves_geometry()
{
    // The constructor inserts knots to unify the seam bases.  The snapshot of
    // the rest state, which lives in that refined basis, must agree with the
    // original surfaces everywhere, not only on the seam.
    std::vector<RationalSurface3d> patches = makePair(2, 0, 1, true, 1.2);
    PatchSeam seam = verticalSeam();
    PatchEditor editor(patches, {seam});
    PatchEditorSnapshot rest = editor.snapshot();
    double worst = 0.0;
    for (std::size_t p = 0; p < patches.size(); ++p)
    {
        const auto u_dom = domainOf(rest.patches[p].knots_u, rest.patches[p].degree_u);
        const auto v_dom = domainOf(rest.patches[p].knots_v, rest.patches[p].degree_v);
        for (int a = 0; a <= 13; ++a)
        {
            for (int b = 0; b <= 13; ++b)
            {
                const double u = u_dom.first + (u_dom.second - u_dom.first) *
                                                    (0.077 + 0.83 * a / 13.0);
                const double v = v_dom.first + (v_dom.second - v_dom.first) *
                                                    (0.043 + 0.91 * b / 13.0);
                worst = std::max(
                    worst, static_cast<double>(glm::length(
                               tinynurbs::surfacePoint(rest.patches[p], u, v) -
                               tinynurbs::surfacePoint(patches[p], u, v))));
            }
        }
    }
    TASK_CHECK(worst < 1e-12);
    return 0;
}

int case_whole_edge_not_sampled()
{
    // After a solve, sample the seam at irrational-looking parameters that do
    // not coincide with the knot stations or with any interpolation points.
    std::vector<RationalSurface3d> patches = makePair(3, 0, 1, true, 0.8);
    PatchSeam seam = verticalSeam();
    seam.reversed = true;
    PatchEditor editor(patches, {seam});
    PatchEditorSnapshot snap = editor.solve();
    double pos = 0.0, der = 0.0;
    seamResidual(snap.patches[0], seam.edge_a, snap.patches[1], seam.edge_b, true, 211, pos, der);
    TASK_CHECK(pos < kTol);
    TASK_CHECK(der < kTol);
    return 0;
}

int case_rigid_and_weight_scale()
{
    // Reference solve.
    std::vector<RationalSurface3d> base = makePair(2, 0, 1, true, 0.7);
    PatchSeam seam = verticalSeam();
    PatchHandle h;
    h.id = 3;
    h.patch = 1;
    h.u = 21.5;
    h.v = 1.0;
    h.target = dvec3(4.0, 2.0, 1.5);
    PatchEditor ref_editor(base, {seam});
    ref_editor.set_handles({h});
    PatchEditorSnapshot ref = ref_editor.solve();

    // Apply a rigid motion and a global weight scale.
    const double angle = 0.7;
    const dvec3 translation(1.5, -2.0, 3.25);
    const double weight_scale = 13.0;
    std::vector<RationalSurface3d> moved = base;
    for (RationalSurface3d &srf : moved)
    {
        for (std::size_t i = 0; i < srf.control_points.rows(); ++i)
        {
            for (std::size_t j = 0; j < srf.control_points.cols(); ++j)
            {
                const dvec3 p = srf.control_points(i, j);
                const dvec3 r(std::cos(angle) * p.x - std::sin(angle) * p.y,
                              std::sin(angle) * p.x + std::cos(angle) * p.y, p.z);
                srf.control_points(i, j) = r + translation;
                srf.weights(i, j) *= weight_scale;
            }
        }
    }
    PatchHandle hm = h;
    hm.target = dvec3(std::cos(angle) * h.target.x - std::sin(angle) * h.target.y,
                      std::sin(angle) * h.target.x + std::cos(angle) * h.target.y, h.target.z) +
                 translation;
    PatchEditor moved_editor(moved, {seam});
    moved_editor.set_handles({hm});
    PatchEditorSnapshot got = moved_editor.solve();

    double worst = 0.0;
    for (std::size_t p = 0; p < ref.patches.size(); ++p)
    {
        for (std::size_t i = 0; i < ref.patches[p].control_points.rows(); ++i)
        {
            for (std::size_t j = 0; j < ref.patches[p].control_points.cols(); ++j)
            {
                const dvec3 rp = ref.patches[p].control_points(i, j);
                const dvec3 expected(std::cos(angle) * rp.x - std::sin(angle) * rp.y,
                                     std::sin(angle) * rp.x + std::cos(angle) * rp.y, rp.z);
                worst = std::max(
                    worst, static_cast<double>(glm::length(
                               got.patches[p].control_points(i, j) - (expected + translation))));
            }
        }
    }
    TASK_CHECK(worst < 1e-7);
    return 0;
}

int case_chain_with_handle()
{
    std::array<RationalSurface3d, 3> chain = makeChain();
    std::vector<RationalSurface3d> patches(chain.begin(), chain.end());
    std::vector<PatchSeam> seams = {
        {0, SurfaceEdge::UMax, 1, SurfaceEdge::UMin, false},
        {1, SurfaceEdge::UMax, 2, SurfaceEdge::UMin, false},
    };
    PatchEditor editor(patches, seams);
    PatchHandle h;
    h.id = 42;
    h.patch = 1;
    h.u = 1.5;
    h.v = 0.6;
    h.target = dvec3(2.0, 3.0, 1.2);
    editor.set_handles({h});
    PatchEditorSnapshot snap = editor.solve();
    for (const PatchSeam &s : seams)
    {
        double pos = 0.0, der = 0.0;
        seamResidual(snap.patches[static_cast<std::size_t>(s.patch_a)], s.edge_a,
                     snap.patches[static_cast<std::size_t>(s.patch_b)], s.edge_b, s.reversed, 64,
                     pos, der);
        TASK_CHECK(pos < kTol);
        TASK_CHECK(der < kTol);
    }
    TASK_CHECK(glm::length(tinynurbs::surfacePoint(snap.patches[1], h.u, h.v) - h.target) < kTol);
    return 0;
}

/////////////////////////////////////////////////////////////////////
// case: projection
/////////////////////////////////////////////////////////////////////

int case_handle_residual()
{
    RationalSurface3d srf = makeSurface(2, 2, clampedKnots(2, -2, 3, {0.5}),
                                        clampedKnots(2, 4, 9, {6.0}), 8, dvec3(0, 0, 0), 0.5);
    std::vector<PatchHandle> handles;
    for (int k = 0; k < 4; ++k)
    {
        PatchHandle h;
        h.id = 10 + k;
        h.patch = 0;
        h.u = -1.0 + 0.9 * k;
        h.v = 5.0 + 0.7 * k;
        h.target = dvec3(0.7 * k, -0.3 * k, 2.0 - 0.4 * k);
        handles.push_back(h);
    }
    PatchEditor editor({srf}, {});
    editor.set_handles(handles);
    PatchEditorSnapshot snap = editor.solve();
    for (const PatchHandle &h : handles)
    {
        TASK_CHECK(glm::length(tinynurbs::surfacePoint(snap.patches[0], h.u, h.v) - h.target) <
                   kTol);
    }
    return 0;
}

int case_independent_projection()
{
    // One degree-2 patch, 3x3 control points, two point handles.  Compare the
    // editor's minimum-displacement result with an independently assembled
    // solution of the same equality-constrained least-squares problem.
    RationalSurface3d srf;
    srf.degree_u = 2;
    srf.degree_v = 2;
    srf.knots_u = {0, 0, 0, 1, 1, 1};
    srf.knots_v = {0, 0, 0, 1, 1, 1};
    srf.control_points.resize(3, 3);
    srf.weights.resize(3, 3);
    for (int i = 0; i < 3; ++i)
    {
        for (int j = 0; j < 3; ++j)
        {
            srf.control_points(i, j) = dvec3(i + 0.1 * j, j - 0.2 * i, 0.3 * i * j);
            srf.weights(i, j) = 0.6 + 0.3 * ((i + 2 * j) % 4);
        }
    }
    PatchHandle h0;
    h0.id = 1;
    h0.patch = 0;
    h0.u = 0.3;
    h0.v = 0.7;
    h0.target = dvec3(-2.0, 5.0, 1.0);
    PatchHandle h1;
    h1.id = 2;
    h1.patch = 0;
    h1.u = 0.85;
    h1.v = 0.2;
    h1.target = dvec3(7.0, -1.0, 3.0);
    std::vector<PatchHandle> handles = {h0, h1};

    PatchEditor editor({srf}, {});
    editor.set_handles(handles);
    PatchEditorSnapshot snap = editor.solve();

    auto basisRow = [&](double u, double v, std::vector<double> &row, double &den) {
        const int su = tinynurbs::findSpan(2, srf.knots_u, u);
        const int sv = tinynurbs::findSpan(2, srf.knots_v, v);
        const std::vector<double> Nu = tinynurbs::bsplineBasis(2, su, srf.knots_u, u);
        const std::vector<double> Nv = tinynurbs::bsplineBasis(2, sv, srf.knots_v, v);
        row.assign(9, 0.0);
        den = 0.0;
        for (int l = 0; l < 3; ++l)
        {
            for (int k = 0; k < 3; ++k)
            {
                const int i = su - 2 + k;
                const int j = sv - 2 + l;
                const double c = srf.weights(i, j) * Nu[static_cast<std::size_t>(k)] *
                                 Nv[static_cast<std::size_t>(l)];
                row[static_cast<std::size_t>(i * 3 + j)] += c;
                den += c;
            }
        }
    };

    std::vector<std::vector<double>> A;
    std::array<std::vector<double>, 3> rhs;
    std::array<std::vector<double>, 3> rest;
    for (int c = 0; c < 3; ++c)
    {
        rhs[static_cast<std::size_t>(c)].assign(2, 0.0);
        rest[static_cast<std::size_t>(c)].assign(9, 0.0);
    }
    for (int k = 0; k < 2; ++k)
    {
        std::vector<double> row;
        double den = 0.0;
        basisRow(handles[static_cast<std::size_t>(k)].u, handles[static_cast<std::size_t>(k)].v,
                 row, den);
        A.push_back(row);
        for (int c = 0; c < 3; ++c)
        {
            rhs[static_cast<std::size_t>(c)][static_cast<std::size_t>(k)] =
                den * handles[static_cast<std::size_t>(k)].target[c];
        }
    }
    for (int i = 0; i < 3; ++i)
    {
        for (int j = 0; j < 3; ++j)
        {
            for (int c = 0; c < 3; ++c)
            {
                rest[static_cast<std::size_t>(c)][static_cast<std::size_t>(i * 3 + j)] =
                    srf.control_points(i, j)[c];
            }
        }
    }

    double G[2][2];
    for (int a = 0; a < 2; ++a)
    {
        for (int b = 0; b < 2; ++b)
        {
            double sum = 0.0;
            for (int k = 0; k < 9; ++k)
            {
                sum += A[static_cast<std::size_t>(a)][static_cast<std::size_t>(k)] *
                       A[static_cast<std::size_t>(b)][static_cast<std::size_t>(k)];
            }
            G[a][b] = sum;
        }
    }
    double worst = 0.0;
    for (int c = 0; c < 3; ++c)
    {
        double b0 = rhs[static_cast<std::size_t>(c)][0];
        double b1 = rhs[static_cast<std::size_t>(c)][1];
        for (int k = 0; k < 9; ++k)
        {
            b0 -= A[0][static_cast<std::size_t>(k)] *
                  rest[static_cast<std::size_t>(c)][static_cast<std::size_t>(k)];
            b1 -= A[1][static_cast<std::size_t>(k)] *
                  rest[static_cast<std::size_t>(c)][static_cast<std::size_t>(k)];
        }
        const double det = G[0][0] * G[1][1] - G[0][1] * G[1][0];
        const double y0 = (b0 * G[1][1] - b1 * G[0][1]) / det;
        const double y1 = (G[0][0] * b1 - G[1][0] * b0) / det;
        for (int k = 0; k < 9; ++k)
        {
            const double expected =
                rest[static_cast<std::size_t>(c)][static_cast<std::size_t>(k)] +
                A[0][static_cast<std::size_t>(k)] * y0 +
                A[1][static_cast<std::size_t>(k)] * y1;
            const double actual =
                snap.patches[0].control_points(static_cast<std::size_t>(k / 3),
                                               static_cast<std::size_t>(k % 3))[c];
            worst = std::max(worst, std::abs(expected - actual));
        }
    }
    TASK_CHECK(worst < 1e-7);
    return 0;
}

int case_rest_preserved_and_release()
{
    // A network whose rest already satisfies the seam must be returned
    // unchanged when no handles are staged.
    std::array<RationalSurface3d, 3> chain = makeChain();
    std::vector<RationalSurface3d> patches(chain.begin(), chain.end());
    std::vector<PatchSeam> seams = {
        {0, SurfaceEdge::UMax, 1, SurfaceEdge::UMin, false},
        {1, SurfaceEdge::UMax, 2, SurfaceEdge::UMin, false},
    };
    PatchEditor editor(patches, seams);
    PatchEditorSnapshot snap = editor.solve();
    double worst = 0.0;
    for (std::size_t p = 0; p < snap.patches.size(); ++p)
    {
        worst = std::max(worst, controlPointDistance(snap.patches[p], patches[p]));
    }
    TASK_CHECK(worst < 1e-12);

    // Drag, then release.  Releasing solves relative to the original rest.
    PatchHandle h;
    h.id = 5;
    h.patch = 1;
    h.u = 1.5;
    h.v = 0.5;
    h.target = dvec3(2.5, 2.5, 2.0);
    editor.set_handles({h});
    PatchEditorSnapshot dragged = editor.solve();
    TASK_CHECK(controlPointDistance(dragged.patches[1], patches[1]) > 1e-3);

    editor.set_handles({});
    PatchEditorSnapshot released = editor.solve();
    worst = 0.0;
    for (std::size_t p = 0; p < released.patches.size(); ++p)
    {
        worst = std::max(worst, controlPointDistance(released.patches[p], patches[p]));
    }
    TASK_CHECK(worst < 1e-10);
    return 0;
}

int case_seam_repair_without_handles()
{
    // Two patches whose rest seam is not continuous.  With no handles the
    // solve must still repair the whole edge (position and cross derivative).
    std::vector<RationalSurface3d> patches = makePair(2, 0, 1, true, 0.9);
    PatchSeam seam = verticalSeam();
    double pos0 = 0.0, der0 = 0.0;
    seamResidual(patches[0], seam.edge_a, patches[1], seam.edge_b, false, 64, pos0, der0);
    TASK_CHECK(pos0 > 1e-3 || der0 > 1e-3);

    PatchEditor editor(patches, {seam});
    PatchEditorSnapshot snap = editor.solve();
    double pos = 0.0, der = 0.0;
    seamResidual(snap.patches[0], seam.edge_a, snap.patches[1], seam.edge_b, false, 64, pos, der);
    TASK_CHECK(pos < kTol);
    TASK_CHECK(der < kTol);
    return 0;
}

int case_redundant_handles()
{
    // Two ids at the same location with the same target are allowed.
    RationalSurface3d srf = makeSurface(2, 2, clampedKnots(2, 0, 1, {}),
                                        clampedKnots(2, 0, 1, {}), 12, dvec3(0, 0, 0), 0.3);
    PatchHandle a;
    a.id = 1;
    a.patch = 0;
    a.u = 0.4;
    a.v = 0.6;
    a.target = dvec3(0.3, 0.3, 1.0);
    PatchHandle b = a;
    b.id = 2;
    PatchEditor editor({srf}, {});
    bool accepted = true;
    try
    {
        editor.set_handles({a, b});
    }
    catch (const std::exception &)
    {
        accepted = false;
    }
    TASK_CHECK(accepted);
    PatchEditorSnapshot snap = editor.solve();
    TASK_CHECK(glm::length(tinynurbs::surfacePoint(snap.patches[0], a.u, a.v) - a.target) < kTol);
    return 0;
}

/////////////////////////////////////////////////////////////////////
// case: lifecycle
/////////////////////////////////////////////////////////////////////

int case_empty_network()
{
    PatchEditor editor({}, {});
    PatchEditorSnapshot snap = editor.solve();
    TASK_CHECK(snap.patches.empty());
    TASK_CHECK(editor.control_point_count() == 0);
    TASK_CHECK(editor.patch_count() == 0);
    return 0;
}

int case_cache_counts()
{
    std::array<RationalSurface3d, 3> chain = makeChain();
    std::vector<RationalSurface3d> patches(chain.begin(), chain.end());
    std::vector<PatchSeam> seams = {
        {0, SurfaceEdge::UMax, 1, SurfaceEdge::UMin, false},
        {1, SurfaceEdge::UMax, 2, SurfaceEdge::UMin, false},
    };
    PatchEditor editor(patches, seams);
    const std::size_t start = editor.factorization_count();

    PatchHandle h;
    h.id = 9;
    h.patch = 1;
    h.u = 1.4;
    h.v = 0.4;
    h.target = dvec3(1.0, 1.0, 2.0);
    editor.set_handles({h});
    editor.solve();
    const std::size_t after_first = editor.factorization_count();
    TASK_CHECK(after_first == start + 1);

    // Only the target changed: the factorization is reused.
    h.target = dvec3(1.2, 1.4, 2.1);
    editor.set_handles({h});
    editor.solve();
    TASK_CHECK(editor.factorization_count() == after_first);

    // The handle location changed: the factorization is rebuilt.
    h.u = 1.6;
    editor.set_handles({h});
    editor.solve();
    const std::size_t after_move = editor.factorization_count();
    TASK_CHECK(after_move == after_first + 1);

    // The handle set changed (an added handle): rebuilt.
    PatchHandle extra;
    extra.id = 10;
    extra.patch = 2;
    extra.u = 2.4;
    extra.v = 0.5;
    extra.target = dvec3(3.0, 2.0, 1.0);
    editor.set_handles({h, extra});
    editor.solve();
    const std::size_t after_add = editor.factorization_count();
    TASK_CHECK(after_add == after_move + 1);

    // Removing all handles: rebuilt for the seam-only system once, then reused.
    editor.set_handles({});
    editor.solve();
    const std::size_t after_empty = editor.factorization_count();
    TASK_CHECK(after_empty == after_add + 1);
    editor.set_handles({});
    editor.solve();
    TASK_CHECK(editor.factorization_count() == after_empty);
    return 0;
}

int case_conflict_restore_and_retry()
{
    std::array<RationalSurface3d, 3> chain = makeChain();
    std::vector<RationalSurface3d> patches(chain.begin(), chain.end());
    std::vector<PatchSeam> seams = {
        {0, SurfaceEdge::UMax, 1, SurfaceEdge::UMin, false},
        {1, SurfaceEdge::UMax, 2, SurfaceEdge::UMin, false},
    };
    PatchEditor editor(patches, seams);

    PatchHandle good;
    good.id = 20;
    good.patch = 1;
    good.u = 1.5;
    good.v = 0.5;
    good.target = dvec3(1.5, 1.5, 2.5);
    editor.set_handles({good});
    PatchEditorSnapshot committed = editor.solve();

    // Two handles on points that the seam forces to coincide but with
    // different targets: each handle is valid on its own patch, so the
    // failure is detected by solve() as an incompatible system.
    PatchHandle a;
    a.id = 30;
    a.patch = 1;
    a.u = 2.0; // UMax seam with patch 2's UMin
    a.v = 0.5;
    a.target = dvec3(0.0, 0.0, 0.0);
    PatchHandle b;
    b.id = 31;
    b.patch = 2;
    b.u = 2.0; // patch 2's UMin, same physical point
    b.v = 0.5;
    b.target = dvec3(1.0, 0.0, 0.0);
    editor.set_handles({a, b});

    bool threw = false;
    try
    {
        editor.solve();
    }
    catch (const std::runtime_error &)
    {
        threw = true;
    }
    TASK_CHECK(threw);

    // State is restored to the last successful solve.
    PatchEditorSnapshot after = editor.snapshot();
    TASK_CHECK(controlPointDistance(after.patches[1], committed.patches[1]) < 1e-14);
    TASK_CHECK(editor.staged_handles().size() == 1);
    TASK_CHECK(editor.staged_handles()[0].id == good.id);

    // A valid retry after the failure succeeds.
    PatchHandle retry = good;
    retry.target = dvec3(1.7, 1.6, 2.4);
    editor.set_handles({retry});
    PatchEditorSnapshot ok = editor.solve();
    TASK_CHECK(glm::length(tinynurbs::surfacePoint(ok.patches[1], retry.u, retry.v) - retry.target) <
               kTol);
    return 0;
}

int case_invalid_inputs()
{
    auto expectInvalid = [](const std::function<void()> &fn) {
        try
        {
            fn();
        }
        catch (const std::invalid_argument &)
        {
            return true;
        }
        catch (const std::exception &)
        {
            return false;
        }
        return false;
    };

    RationalSurface3d good = makeSurface(2, 2, clampedKnots(2, 0, 1, {}),
                                         clampedKnots(2, 0, 1, {}), 1, dvec3(0, 0, 0), 0.3);

    // Degree out of range.
    RationalSurface3d deg4 = makeSurface(2, 2, {0, 0, 0, 0, 0, 1, 1, 1, 1, 1},
                                          clampedKnots(2, 0, 1, {}), 2, dvec3(0, 0, 0), 0.2);
    deg4.degree_u = 4;
    TASK_CHECK(expectInvalid([&]() { PatchEditor e({deg4}, {}); }));

    // Non-clamped knots.
    RationalSurface3d unclamped = good;
    unclamped.knots_u = {0, 0.1, 0.2, 1, 1, 1};
    TASK_CHECK(expectInvalid([&]() { PatchEditor e({unclamped}, {}); }));

    // Interior multiplicity greater than the degree.
    RationalSurface3d mult = makeSurface(2, 2, {0, 0, 0, 0.5, 0.5, 0.5, 1, 1, 1},
                                         clampedKnots(2, 0, 1, {}), 3, dvec3(0, 0, 0), 0.2);
    TASK_CHECK(expectInvalid([&]() { PatchEditor e({mult}, {}); }));

    // Non-positive weight.
    RationalSurface3d badw = good;
    badw.weights(1, 1) = 0.0;
    TASK_CHECK(expectInvalid([&]() { PatchEditor e({badw}, {}); }));

    // Non-finite coordinate.
    RationalSurface3d badcp = good;
    badcp.control_points(1, 1).x = std::numeric_limits<double>::quiet_NaN();
    TASK_CHECK(expectInvalid([&]() { PatchEditor e({badcp}, {}); }));

    // Invalid seam edge value and out-of-range patch index.
    TASK_CHECK(expectInvalid([&]() {
        PatchEditor e({good, good}, {{0, static_cast<SurfaceEdge>(9), 1, SurfaceEdge::UMin,
                                      false}});
    }));
    TASK_CHECK(expectInvalid([&]() {
        PatchEditor e({good}, {{0, SurfaceEdge::UMax, 3, SurfaceEdge::UMin, false}});
    }));

    // A patch edge may be used by at most one seam.
    TASK_CHECK(expectInvalid([&]() {
        PatchEditor e({good, good, good},
                      {{0, SurfaceEdge::UMax, 1, SurfaceEdge::UMin, false},
                       {0, SurfaceEdge::UMax, 2, SurfaceEdge::UMin, false}});
    }));

    // Duplicate handle id.
    PatchEditor editor({good}, {});
    PatchHandle h;
    h.id = 5;
    h.patch = 0;
    h.u = 0.5;
    h.v = 0.5;
    h.target = dvec3(0, 0, 0);
    PatchHandle dup = h;
    TASK_CHECK(expectInvalid([&]() { editor.set_handles({h, dup}); }));

    // Same point with different targets.
    PatchHandle other = h;
    other.id = 6;
    other.target = dvec3(1, 1, 1);
    TASK_CHECK(expectInvalid([&]() { editor.set_handles({h, other}); }));

    // Handle outside the patch domain.
    PatchHandle outside = h;
    outside.id = 7;
    outside.u = 9.0;
    TASK_CHECK(expectInvalid([&]() { editor.set_handles({outside}); }));

    // Non-finite target.
    PatchHandle nan = h;
    nan.id = 8;
    nan.target.x = std::numeric_limits<double>::infinity();
    TASK_CHECK(expectInvalid([&]() { editor.set_handles({nan}); }));

    // A rejected set_handles must not overwrite the staged handles.
    editor.set_handles({h});
    const std::size_t staged = editor.staged_handles().size();
    try
    {
        editor.set_handles({h, other});
    }
    catch (const std::invalid_argument &)
    {
    }
    TASK_CHECK(editor.staged_handles().size() == staged);
    TASK_CHECK(editor.staged_handles()[0].id == h.id);
    return 0;
}

int case_continuous_drag()
{
    std::array<RationalSurface3d, 3> chain = makeChain();
    std::vector<RationalSurface3d> patches(chain.begin(), chain.end());
    std::vector<PatchSeam> seams = {
        {0, SurfaceEdge::UMax, 1, SurfaceEdge::UMin, false},
        {1, SurfaceEdge::UMax, 2, SurfaceEdge::UMin, false},
    };
    PatchEditor editor(patches, seams);
    const dvec3 start_target(1.0, 1.0, 1.0);
    PatchHandle h;
    h.id = 55;
    h.patch = 1;
    h.u = 1.5;
    h.v = 0.5;
    h.target = start_target;

    for (int step = 1; step <= 6; ++step)
    {
        h.target = start_target + dvec3(0.15 * step, -0.1 * step, 0.2 * step);
        editor.set_handles({h});
        PatchEditorSnapshot snap = editor.solve();
        TASK_CHECK(glm::length(tinynurbs::surfacePoint(snap.patches[1], h.u, h.v) - h.target) <
                   kTol);
        for (const PatchSeam &s : seams)
        {
            double pos = 0.0, der = 0.0;
            seamResidual(snap.patches[static_cast<std::size_t>(s.patch_a)], s.edge_a,
                         snap.patches[static_cast<std::size_t>(s.patch_b)], s.edge_b, s.reversed,
                         24, pos, der);
            TASK_CHECK(pos < kTol);
            TASK_CHECK(der < kTol);
        }
    }
    return 0;
}

/////////////////////////////////////////////////////////////////////
// Mesh checks
/////////////////////////////////////////////////////////////////////

std::map<std::pair<int, int>, int> triangleEdgeUse(const PatchMesh &mesh,
                                                   std::vector<int> &patch_of_edge)
{
    std::map<std::pair<int, int>, int> use;
    std::map<std::pair<int, int>, int> patch;
    for (const auto &tri : mesh.triangles)
    {
        for (int c = 0; c < 3; ++c)
        {
            int a = tri.corners[c].position;
            int b = tri.corners[(c + 1) % 3].position;
            if (a > b)
            {
                std::swap(a, b);
            }
            const std::pair<int, int> key(a, b);
            if (use.find(key) == use.end())
            {
                patch[key] = tri.corners[c].patch;
            }
            use[key] += 1;
        }
    }
    patch_of_edge.clear();
    for (const auto &entry : use)
    {
        patch_of_edge.push_back(patch[entry.first]);
    }
    return use;
}

int case_mesh_adjacency()
{
    std::array<RationalSurface3d, 3> chain = makeChain();
    std::vector<RationalSurface3d> patches(chain.begin(), chain.end());
    std::vector<PatchSeam> seams = {
        {0, SurfaceEdge::UMax, 1, SurfaceEdge::UMin, false},
        {1, SurfaceEdge::UMax, 2, SurfaceEdge::UMin, false},
    };
    PatchEditor editor(patches, seams);
    PatchEditorSnapshot snap = editor.solve();
    PatchMesh mesh = tinynurbs::buildPatchMesh(snap);
    TASK_CHECK(!mesh.triangles.empty());
    TASK_CHECK(!mesh.positions.empty());

    // Every triangle corner index is in range and its patch index is valid.
    for (const auto &tri : mesh.triangles)
    {
        for (int c = 0; c < 3; ++c)
        {
            TASK_CHECK(tri.corners[c].position >= 0);
            TASK_CHECK(static_cast<std::size_t>(tri.corners[c].position) < mesh.positions.size());
            TASK_CHECK(tri.corners[c].patch >= 0 &&
                       static_cast<std::size_t>(tri.corners[c].patch) < snap.patches.size());
        }
    }

    // Count edges that are shared by two triangles belonging to different
    // patches; there must be at least one per declared seam and no other
    // cross-patch sharing.
    std::map<std::pair<int, int>, std::set<int>> edge_patches;
    for (const auto &tri : mesh.triangles)
    {
        for (int c = 0; c < 3; ++c)
        {
            int a = tri.corners[c].position;
            int b = tri.corners[(c + 1) % 3].position;
            if (a > b)
            {
                std::swap(a, b);
            }
            edge_patches[{a, b}].insert(tri.corners[c].patch);
        }
    }
    std::set<std::pair<int, int>> welded_pairs;
    for (const auto &entry : edge_patches)
    {
        if (entry.second.size() == 2)
        {
            std::vector<int> p(entry.second.begin(), entry.second.end());
            welded_pairs.insert({p[0], p[1]});
        }
        TASK_CHECK(entry.second.size() <= 2);
    }
    std::set<std::pair<int, int>> declared;
    for (const PatchSeam &s : seams)
    {
        declared.insert({std::min(s.patch_a, s.patch_b), std::max(s.patch_a, s.patch_b)});
    }
    TASK_CHECK(welded_pairs == declared);
    return 0;
}

int case_mesh_undeclared_not_welded()
{
    // Two patches that touch geometrically but have no seam declaration.
    RationalSurface3d A = makeSurface(2, 2, clampedKnots(2, 0, 1, {}),
                                      clampedKnots(2, 0, 1, {}), 20, dvec3(0, 0, 0), 0.0);
    RationalSurface3d B = A;
    PatchEditor editor({A, B}, {});
    PatchEditorSnapshot snap = editor.solve();
    PatchMesh mesh = tinynurbs::buildPatchMesh(snap);

    // No welding at all: the number of positions equals the total grid points.
    PatchMesh single_a = tinynurbs::buildPatchMesh(
        [&]() {
            PatchEditor one({A}, {});
            return one.solve();
        }());
    TASK_CHECK(mesh.positions.size() == 2 * single_a.positions.size());

    // Every welded position is used by exactly one patch.
    std::map<int, std::set<int>> pos_patch;
    for (const auto &tri : mesh.triangles)
    {
        for (int c = 0; c < 3; ++c)
        {
            pos_patch[tri.corners[c].position].insert(tri.corners[c].patch);
        }
    }
    for (const auto &entry : pos_patch)
    {
        TASK_CHECK(entry.second.size() == 1);
    }
    return 0;
}

int case_mesh_corner_attributes()
{
    // After welding, the shared seam vertices still carry each patch's own
    // original UV and an analytic normal.
    std::array<RationalSurface3d, 3> chain = makeChain();
    std::vector<RationalSurface3d> patches(chain.begin(), chain.end());
    std::vector<PatchSeam> seams = {
        {0, SurfaceEdge::UMax, 1, SurfaceEdge::UMin, false},
        {1, SurfaceEdge::UMax, 2, SurfaceEdge::UMin, false},
    };
    PatchEditor editor(patches, seams);
    PatchEditorSnapshot snap = editor.solve();
    PatchMesh mesh = tinynurbs::buildPatchMesh(snap);

    // Collect, per welded position, the set of (patch, u, v) corners.
    std::map<int, std::set<int>> patches_at;
    std::map<int, std::vector<tinynurbs::PatchMeshCorner>> corners_at;
    for (const auto &tri : mesh.triangles)
    {
        for (int c = 0; c < 3; ++c)
        {
            const auto &corner = tri.corners[c];
            patches_at[corner.position].insert(corner.patch);
            corners_at[corner.position].push_back(corner);
            TASK_CHECK(std::abs(glm::length(corner.normal) - 1.0) < 1e-6 ||
                       glm::length(corner.normal) == 0.0);
        }
    }
    std::size_t shared = 0;
    for (const auto &entry : patches_at)
    {
        if (entry.second.size() != 2)
        {
            continue;
        }
        ++shared;
        // Each side's corner must evaluate back to the shared position, keep
        // its own analytic normal, and store a UV that lies in *its own*
        // patch's original parameter domain.  The two sides may have equal
        // numeric UVs when the patches share a common parameter space, but
        // the stored pair always belongs to that corner's patch.
        std::set<std::pair<int, std::pair<double, double>>> uvs;
        for (const auto &corner : corners_at[entry.first])
        {
            const auto &srf = snap.patches[static_cast<std::size_t>(corner.patch)];
            const dvec3 p = tinynurbs::surfacePoint(srf, corner.u, corner.v);
            TASK_CHECK(glm::length(p - mesh.positions[static_cast<std::size_t>(corner.position)]) <
                       1e-7);
            TASK_CHECK(std::abs(glm::length(corner.normal) - 1.0) < 1e-6);
            const auto u_dom = domainOf(srf.knots_u, srf.degree_u);
            const auto v_dom = domainOf(srf.knots_v, srf.degree_v);
            TASK_CHECK(corner.u >= u_dom.first - 1e-12 && corner.u <= u_dom.second + 1e-12);
            TASK_CHECK(corner.v >= v_dom.first - 1e-12 && corner.v <= v_dom.second + 1e-12);
            uvs.insert({corner.patch, {corner.u, corner.v}});
        }
        TASK_CHECK(uvs.size() == 2);
        auto it = uvs.begin();
        const auto first = *it;
        ++it;
        const auto second = *it;
        TASK_CHECK(first.first != second.first);
    }
    TASK_CHECK(shared > 0);
    return 0;
}

int case_mesh_grid_density()
{
    // Every original knot cell is split into a 4x4 grid of quads (32
    // triangles).  A single patch with one cell in each direction and no
    // seam therefore has 16 quads and a 5x5 position grid.
    RationalSurface3d single = makeSurface(2, 2, clampedKnots(2, 0, 1, {}),
                                           clampedKnots(2, 0, 1, {}), 21, dvec3(0, 0, 0), 0.2);
    PatchEditor one_editor({single}, {});
    PatchEditorSnapshot one = one_editor.solve();
    PatchMesh one_mesh = tinynurbs::buildPatchMesh(one);
    TASK_CHECK(one_mesh.triangles.size() == 2 * 16);
    TASK_CHECK(one_mesh.positions.size() == 25);

    // Two cells along u and three along v gives 2*3 cells.
    RationalSurface3d multi = makeSurface(
        2, 2, clampedKnots(2, 0, 2, {0.9}), clampedKnots(2, 0, 3, {0.7, 1.9}), 22,
        dvec3(0, 0, 0), 0.2);
    PatchEditor multi_editor({multi}, {});
    PatchEditorSnapshot m = multi_editor.solve();
    PatchMesh multi_mesh = tinynurbs::buildPatchMesh(m);
    TASK_CHECK(multi_mesh.triangles.size() == 2 * 16 * (2 * 3));
    TASK_CHECK(multi_mesh.positions.size() == static_cast<std::size_t>((2 * 4 + 1) * (3 * 4 + 1)));
    return 0;
}

} // namespace

/////////////////////////////////////////////////////////////////////

const struct task_case task_cases[] = {
    {"continuity", "degrees_domains_reversal", case_degrees_domains_reversal},
    {"continuity", "cross_axis", case_cross_axis},
    {"continuity", "four_around_corner", case_four_around_corner},
    {"continuity", "samples_only_match", case_samples_only_match},
    {"continuity", "refinement_preserves_geometry", case_refinement_preserves_geometry},
    {"continuity", "whole_edge_not_sampled", case_whole_edge_not_sampled},
    {"continuity", "rigid_and_weight_scale", case_rigid_and_weight_scale},
    {"continuity", "chain_with_handle", case_chain_with_handle},
    {"projection", "handle_residual", case_handle_residual},
    {"projection", "independent_projection", case_independent_projection},
    {"projection", "rest_preserved_and_release", case_rest_preserved_and_release},
    {"projection", "seam_repair_without_handles", case_seam_repair_without_handles},
    {"projection", "redundant_handles", case_redundant_handles},
    {"lifecycle", "empty_network", case_empty_network},
    {"lifecycle", "cache_counts", case_cache_counts},
    {"lifecycle", "conflict_restore_and_retry", case_conflict_restore_and_retry},
    {"lifecycle", "invalid_inputs", case_invalid_inputs},
    {"lifecycle", "continuous_drag", case_continuous_drag},
    {"lifecycle", "mesh_adjacency", case_mesh_adjacency},
    {"lifecycle", "mesh_undeclared_not_welded", case_mesh_undeclared_not_welded},
    {"lifecycle", "mesh_corner_attributes", case_mesh_corner_attributes},
    {"lifecycle", "mesh_grid_density", case_mesh_grid_density},
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
