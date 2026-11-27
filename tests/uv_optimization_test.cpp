// Copyright 2026 the Polygon Mesh Processing Library developers.
// SPDX-License-Identifier: MIT

// Standalone checks for pmp::optimize_uv. The six entry points declared in
// checks/target_main.cpp are defined in the uv_tests namespace. Every
// reference computation below is independent of the production solver:
// per-face Jacobians, energies and gradients are re-derived here from the
// 3D triangle geometry, and gradients are checked with central finite
// differences.

#include "pmp/algorithms/uv_optimization.h"
#include "pmp/exceptions.h"

#include <cmath>
#include <cstring>
#include <functional>
#include <iostream>
#include <limits>
#include <random>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace uv_tests {

namespace {

using pmp::Face;
using pmp::InvalidInputException;
using pmp::Point;
using pmp::SurfaceMesh;
using pmp::TexCoord;
using pmp::UvOptimizationResult;
using pmp::Vertex;
using pmp::VertexProperty;

constexpr double PI = 3.14159265358979323846;

void check(bool condition, const std::string& message)
{
    if (!condition)
        throw std::runtime_error("uv test failed: " + message);
}

void check_close(double a, double b, double tol, const std::string& what)
{
    if (std::abs(a - b) > tol)
        throw std::runtime_error("uv test failed: " + what +
                                 " expected " + std::to_string(b) + " got " +
                                 std::to_string(a));
}

// Structured annular triangle grid with nr rings (ring 0 = outer boundary,
// ring nr-1 = inner boundary) and nseg segments per ring.
struct RingMesh
{
    SurfaceMesh mesh;
    std::vector<std::vector<Vertex>> v;
    int nr{0};
    int nseg{0};
};

RingMesh make_ring(int nr, int nseg, double r_outer, double dr,
                   const std::function<Point(int, int, double, double)>& pos)
{
    RingMesh rm;
    rm.nr = nr;
    rm.nseg = nseg;
    rm.v.assign(nr, std::vector<Vertex>(nseg));
    for (int r = 0; r < nr; ++r)
        for (int i = 0; i < nseg; ++i)
        {
            const double a = 2.0 * PI * i / nseg;
            const double rad = r_outer - r * dr;
            rm.v[r][i] = rm.mesh.add_vertex(pos(r, i, rad, a));
        }
    for (int r = 0; r + 1 < nr; ++r)
        for (int i = 0; i < nseg; ++i)
        {
            const int j = (i + 1) % nseg;
            rm.mesh.add_triangle(rm.v[r][i], rm.v[r][j], rm.v[r + 1][j]);
            rm.mesh.add_triangle(rm.v[r][i], rm.v[r + 1][j],
                                 rm.v[r + 1][i]);
        }
    return rm;
}

// ----------------------------------------------------------------------
// Independent reference implementation (no production energy code reused)
// ----------------------------------------------------------------------

struct RefFace
{
    Vertex v[3];
    double inv00, inv01, inv10, inv11;
    double area;
};

std::vector<RefFace> reference_faces(const SurfaceMesh& mesh)
{
    auto points = mesh.get_vertex_property<Point>("v:point");
    std::vector<RefFace> out;
    for (auto f : mesh.faces())
    {
        auto it = mesh.vertices(f);
        const Vertex v0 = *it;
        ++it;
        const Vertex v1 = *it;
        ++it;
        const Vertex v2 = *it;
        const Point p0 = points[v0];
        const Point p1 = points[v1];
        const Point p2 = points[v2];

        Point e1 = p1 - p0;
        const double l01 = pmp::norm(e1);
        e1 /= l01;
        const Point raw = p2 - p0;
        const double q2x = pmp::dot(raw, e1);
        const Point perp = raw - q2x * e1;
        const double q2y = pmp::norm(perp);
        const double detD = l01 * q2y;

        RefFace rf;
        rf.v[0] = v0;
        rf.v[1] = v1;
        rf.v[2] = v2;
        rf.area = 0.5 * detD;
        rf.inv00 = q2y / detD;
        rf.inv01 = -q2x / detD;
        rf.inv10 = 0.0;
        rf.inv11 = l01 / detD;
        out.push_back(rf);
    }
    return out;
}

// Row-major 2x2 Jacobian for one face, computed with plain arithmetic.
std::array<double, 4> ref_jacobian(const RefFace& rf,
                                   const VertexProperty<TexCoord>& tex)
{
    const TexCoord u0 = tex[rf.v[0]];
    const TexCoord u1 = tex[rf.v[1]];
    const TexCoord u2 = tex[rf.v[2]];
    const double d1x = u1[0] - u0[0];
    const double d1y = u1[1] - u0[1];
    const double d2x = u2[0] - u0[0];
    const double d2y = u2[1] - u0[1];
    return {d1x * rf.inv00 + d2x * rf.inv10,
            d1x * rf.inv01 + d2x * rf.inv11,
            d1y * rf.inv00 + d2y * rf.inv10,
            d1y * rf.inv01 + d2y * rf.inv11};
}

double ref_density(const std::array<double, 4>& J)
{
    const double det = J[0] * J[3] - J[1] * J[2];
    const double idet = 1.0 / det;
    // J^{-1} = 1/det * [[d,-b],[-c,a]]
    const double ia = J[3] * idet;
    const double ib = -J[1] * idet;
    const double ic = -J[2] * idet;
    const double id = J[0] * idet;
    const double n2 = J[0] * J[0] + J[1] * J[1] + J[2] * J[2] + J[3] * J[3];
    const double ni2 = ia * ia + ib * ib + ic * ic + id * id;
    return n2 + ni2;
}

double ref_energy(const SurfaceMesh& mesh,
                  const VertexProperty<TexCoord>& tex)
{
    const auto faces = reference_faces(mesh);
    double num = 0.0;
    double area = 0.0;
    for (const auto& rf : faces)
    {
        const double e = ref_density(ref_jacobian(rf, tex));
        num += rf.area * e;
        area += rf.area;
    }
    return num / area;
}

// Central-difference gradient of the reference energy on the free UVs.
double ref_gradient_inf_norm(const SurfaceMesh& mesh,
                             const VertexProperty<TexCoord>& tex,
                             const std::vector<char>& is_free)
{
    double best = 0.0;
    for (auto v : mesh.vertices())
    {
        if (!is_free[v.idx()])
            continue;
        for (int c = 0; c < 2; ++c)
        {
            const double original = tex[v][c];
            const double h = 1e-7 * std::max(1.0, std::abs(original));
            const_cast<VertexProperty<TexCoord>&>(tex)[v][c] = original + h;
            const double ep = ref_energy(mesh, tex);
            const_cast<VertexProperty<TexCoord>&>(tex)[v][c] = original - h;
            const double em = ref_energy(mesh, tex);
            const_cast<VertexProperty<TexCoord>&>(tex)[v][c] = original;
            best = std::max(best, std::abs((ep - em) / (2.0 * h)));
        }
    }
    return best;
}

std::vector<char> free_vertices(const SurfaceMesh& mesh)
{
    const bool has_fixed = mesh.has_vertex_property("v:uv_fixed");
    VertexProperty<bool> pinned;
    if (has_fixed)
        pinned = mesh.get_vertex_property<bool>("v:uv_fixed");
    const bool use_pinned = static_cast<bool>(pinned);
    std::vector<char> free(mesh.vertices_size(), char{0});
    for (auto v : mesh.vertices())
    {
        if (mesh.is_isolated(v))
            continue;
        if (mesh.is_boundary(v))
            continue;
        if (use_pinned && pinned[v])
            continue;
        free[v.idx()] = 1;
    }
    return free;
}

std::vector<TexCoord> snapshot_uv(const SurfaceMesh& mesh)
{
    auto tex = mesh.get_vertex_property<TexCoord>("v:tex");
    std::vector<TexCoord> s(mesh.vertices_size());
    for (auto v : mesh.vertices())
        s[v.idx()] = tex[v];
    return s;
}

void check_uv_unchanged(const SurfaceMesh& mesh,
                        const std::vector<TexCoord>& s,
                        const std::string& what)
{
    auto tex = mesh.get_vertex_property<TexCoord>("v:tex");
    for (auto v : mesh.vertices())
        // bitwise compare so NaN payloads are compared correctly
        if (std::memcmp(&tex[v], &s[v.idx()], sizeof(TexCoord)) != 0)
            throw std::runtime_error("uv test failed: UV changed in " +
                                     what);
}

void check_result_invariants(const UvOptimizationResult& r,
                             unsigned int max_iterations)
{
    check(r.energies.size() == r.accepted_steps + 1,
          "energies size must be accepted_steps + 1");
    check(r.accepted_steps <= max_iterations, "too many accepted steps");
    for (auto e : r.energies)
        check(std::isfinite(e), "non-finite energy reported");
    for (size_t k = 1; k < r.energies.size(); ++k)
    {
        const double tol = 1e-12 * std::max(1.0, r.energies[k - 1]);
        if (r.energies[k] > r.energies[k - 1] + tol)
            throw std::runtime_error("uv test failed: energy increased");
    }
    check(std::isfinite(r.gradient_norm), "non-finite gradient norm");
}

bool all_faces_positive(const SurfaceMesh& mesh)
{
    auto tex = mesh.get_vertex_property<TexCoord>("v:tex");
    for (const auto& rf : reference_faces(mesh))
    {
        const auto J = ref_jacobian(rf, tex);
        const double det = J[0] * J[3] - J[1] * J[2];
        if (!(det > 0.0) || !std::isfinite(det))
            return false;
    }
    return true;
}

} // anonymous namespace

// ----------------------------------------------------------------------
// 1. Energy and gradient against an independent finite-difference reference
// ----------------------------------------------------------------------

void energy_gradient()
{
    // Folded, non-coplanar patch: two triangles of clearly different area.
    SurfaceMesh mesh;
    const Vertex a = mesh.add_vertex(Point(0, 0, 0));
    const Vertex b = mesh.add_vertex(Point(4, 0, 0.2));
    const Vertex c = mesh.add_vertex(Point(4, 3, -0.3));
    const Vertex d = mesh.add_vertex(Point(0, 3, 0.5));
    const Vertex e = mesh.add_vertex(Point(2, 1.2, 1.0)); // interior-ish
    mesh.add_triangle(a, b, e);
    mesh.add_triangle(b, c, e);
    mesh.add_triangle(c, d, e);
    mesh.add_triangle(d, a, e);
    // a,b,c,d are boundary, e is the only interior (free) vertex.

    auto tex = mesh.vertex_property<TexCoord>("v:tex");
    tex[a] = TexCoord(0, 0);
    tex[b] = TexCoord(5, 0);
    tex[c] = TexCoord(5, 4);
    tex[d] = TexCoord(0, 4);
    tex[e] = TexCoord(2.7, 1.6); // deliberately off-center

    check(all_faces_positive(mesh), "initial chart must be positive");
    const auto uv_before = snapshot_uv(mesh);
    const double e_before = ref_energy(mesh, tex);
    check_close(4.0, e_before, 1.0, "sanity: distorted energy above 4");
    check(e_before > 4.0, "distorted start should exceed the isometry value");

    auto result = optimize_uv(mesh, 100, 1e-10);
    check_result_invariants(result, 100);

    // energies[0] matches the independently computed input energy
    check_close(result.energies.front(), e_before, 1e-11,
                "reported initial energy");
    check_close(result.energies.back(), ref_energy(mesh, tex), 1e-11,
                "reported final energy");
    check(result.energies.back() <= result.energies.front(),
          "optimization must not increase energy");

    // Boundary pins kept component-wise.
    for (auto v : {a, b, c, d})
        for (int k = 0; k < 2; ++k)
            check(tex[v][k] == uv_before[v.idx()][k],
                  "boundary vertex moved");

    // Independent gradient via central differences at the final state.
    const auto is_free = free_vertices(mesh);
    const double fd_norm = ref_gradient_inf_norm(mesh, tex, is_free);
    check_close(result.gradient_norm, fd_norm, 1e-7,
                "analytic gradient norm vs central differences");
    check(all_faces_positive(mesh), "chart flipped during optimization");

    // A planar identity chart is an exact isometry: energy exactly 4.
    SurfaceMesh flat;
    const Vertex f0 = flat.add_vertex(Point(0, 0, 0));
    const Vertex f1 = flat.add_vertex(Point(1, 0, 0));
    const Vertex f2 = flat.add_vertex(Point(1, 1, 0));
    const Vertex f3 = flat.add_vertex(Point(0, 1, 0));
    flat.add_triangle(f0, f1, f2);
    flat.add_triangle(f0, f2, f3);
    auto ftex = flat.vertex_property<TexCoord>("v:tex");
    for (auto v : flat.vertices())
        ftex[v] =
            TexCoord(flat.position(v)[0], flat.position(v)[1]);
    auto iso = optimize_uv(flat, 50, 1e-12);
    check_close(iso.energies.front(), 4.0, 1e-12,
                "planar isometry energy must be 4");
    check(iso.accepted_steps == 0, "isometric input needs no steps");
    check(iso.gradient_norm <= 1e-10, "isometric gradient must vanish");
    check(iso.converged, "isometric input is converged");
}

// ----------------------------------------------------------------------
// 2. Distortion reduction and analytic planar convergence
// ----------------------------------------------------------------------

void distortion_reduction()
{
    // Asymmetric annular grid: 5 rings (3 free rings), 13 segments,
    // non-uniform 3D shape.
    auto rm = make_ring(5, 13, 5.0, 1.0,
                        [](int r, int i, double rad, double a) {
                            (void)r;
                            (void)i;
                            return Point(rad * std::cos(a),
                                         rad * std::sin(a),
                                         0.7 * std::sin(2.0 * a) +
                                             0.3 * rad);
                        });
    auto tex = rm.mesh.vertex_property<TexCoord>("v:tex");
    std::mt19937 rng(42);
    std::normal_distribution<> noise(0.0, 0.35);
    // Smooth plus random interior displacement; scale it down until the
    // distorted chart is provably orientation-preserving on every face.
    double amp = 1.0;
    for (int attempt = 0; attempt < 12; ++attempt)
    {
        for (int r = 0; r < rm.nr; ++r)
            for (int i = 0; i < rm.nseg; ++i)
            {
                const double a = 2.0 * PI * i / rm.nseg;
                const double rad = 5.0 - r;
                const bool interior = (r > 0 && r < rm.nr - 1);
                const double jx =
                    interior ? amp * noise(rng) : 0.0;
                const double jy =
                    interior ? amp * 0.5 * std::sin(3.0 * a) *
                                   (r == 2 ? 1.0 : 0.5)
                             : 0.0;
                tex[rm.v[r][i]] =
                    TexCoord(rad * std::cos(a) + jx,
                             rad * std::sin(a) + jy);
            }
        if (all_faces_positive(rm.mesh))
            break;
        amp *= 0.5;
    }
    check(all_faces_positive(rm.mesh), "start chart must be positive");
    const double e0 = ref_energy(rm.mesh, tex);
    check(e0 > 4.5, "start chart should be visibly distorted");

    auto result = optimize_uv(rm.mesh, 200, 1e-9);
    check_result_invariants(result, 200);
    check(result.accepted_steps <= 200, "step budget exceeded");
    const double drop = (e0 - result.energies.back()) / e0;
    check(drop >= 0.05, "energy must drop by at least 5%");
    check(all_faces_positive(rm.mesh), "chart flipped");

    // Regular planar annulus: the XY chart is the analytic minimizer with
    // energy 4. Perturb interior UVs, then require convergence back to it.
    auto planar = make_ring(4, 16, 4.0, 1.0,
                            [](int, int, double rad, double a) {
                                return Point(rad * std::cos(a),
                                             rad * std::sin(a), 0.0);
                            });
    auto ptex = planar.mesh.vertex_property<TexCoord>("v:tex");
    for (int r = 0; r < planar.nr; ++r)
        for (int i = 0; i < planar.nseg; ++i)
        {
            const double a = 2.0 * PI * i / planar.nseg;
            const double rad = 4.0 - r;
            const double jx =
                (r > 0 && r < planar.nr - 1) ? 0.4 * std::sin(3.0 * a) : 0.0;
            const double jy =
                (r > 0 && r < planar.nr - 1) ? 0.3 * std::cos(2.0 * a) : 0.0;
            ptex[planar.v[r][i]] =
                TexCoord(rad * std::cos(a) + jx, rad * std::sin(a) + jy);
        }
    auto pres = optimize_uv(planar.mesh, 200, 1e-8);
    check_result_invariants(pres, 200);
    check_close(pres.energies.back(), 4.0, 1e-10,
                "planar annulus must converge to the isometric energy 4");
    check(pres.converged, "regular planar case should report convergence");
    check(pres.gradient_norm <= 1e-8, "planar gradient threshold honored");
    check(all_faces_positive(planar.mesh), "planar chart flipped");
}

// ----------------------------------------------------------------------
// 3. Flip prevention and the standalone step-limit helper
// ----------------------------------------------------------------------

void no_flip()
{
    // Independent direction whose determinant is positive at t=0 and at a
    // later endpoint but negative in between (two positive roots).
    // det(J + t dJ) = t^2 - 3t + 2 = (t-1)(t-2)
    const std::array<double, 4> J = {2.0, 0.0, 0.0, 1.0};
    const std::array<double, 4> dJ = {-1.0, 0.0, 0.0, -1.0};
    auto poly = [&](double t) {
        const double a = J[0] + t * dJ[0];
        const double b = J[1] + t * dJ[1];
        const double c = J[2] + t * dJ[2];
        const double d = J[3] + t * dJ[3];
        return a * d - b * c;
    };
    check(poly(0.0) > 0.0, "polynomial setup: det at t=0 positive");
    check(poly(0.5) > 0.0, "first endpoint must be positive");
    check(poly(1.5) < 0.0, "interior must be negative");
    check(poly(2.5) > 0.0, "second endpoint must be positive");

    const double limit = pmp::detail::uv_positive_step_limit(J, dJ);
    check_close(limit, 1.0, 1e-12, "first positive root must be blocked");

    // Linear case: det = 1 - t, single root at t = 1.
    const std::array<double, 4> Jid = {1, 0, 0, 1};
    const std::array<double, 4> dlin = {-1, 0, 0, 0};
    check_close(pmp::detail::uv_positive_step_limit(Jid, dlin), 1.0, 1e-12,
                "linear root");

    // No positive root: identity growth keeps det positive forever.
    const std::array<double, 4> dgrow = {1, 0, 0, 1};
    const double inf =
        pmp::detail::uv_positive_step_limit(Jid, dgrow);
    check(std::isinf(inf) && inf > 0.0, "expected positive infinity");
    const std::array<double, 4> dzero = {0, 0, 0, 0};
    check(std::isinf(pmp::detail::uv_positive_step_limit(Jid, dzero)),
          "zero direction has no root");

    // Near-degenerate initial chart: regular 3D ring but the inner UV loop
    // collapses onto a tiny but strictly positive circle.
    auto rm = make_ring(3, 12, 3.0, 1.0,
                        [](int, int, double rad, double a) {
                            return Point(rad * std::cos(a),
                                         rad * std::sin(a), 0.0);
                        });
    auto tex = rm.mesh.vertex_property<TexCoord>("v:tex");
    for (int r = 0; r < rm.nr; ++r)
        for (int i = 0; i < rm.nseg; ++i)
        {
            const double a = 2.0 * PI * i / rm.nseg;
            const double rad = 3.0 - r;
            const double uv_rad = (r == rm.nr - 1) ? 1e-10 : rad;
            tex[rm.v[r][i]] =
                TexCoord(uv_rad * std::cos(a), uv_rad * std::sin(a));
        }
    check(all_faces_positive(rm.mesh),
          "near-flip start must still be strictly positive");
    double min_det = std::numeric_limits<double>::infinity();
    for (const auto& rf : reference_faces(rm.mesh))
        min_det = std::min(min_det, [&] {
            const auto JJ = ref_jacobian(rf, tex);
            return JJ[0] * JJ[3] - JJ[1] * JJ[2];
        }());
    check(min_det > 0.0 && min_det < 1e-9, "need a genuinely near-flip face");

    auto result = optimize_uv(rm.mesh, 60, 1e-9);
    check_result_invariants(result, 60);
    check(all_faces_positive(rm.mesh),
          "near-flip optimization must not cross zero");

    // Genuinely thin 3D sliver triangles: height 1e-9, isometric UV chart.
    SurfaceMesh sliver;
    const Vertex s0 = sliver.add_vertex(Point(0, 0, 0));
    const Vertex s1 = sliver.add_vertex(Point(1, 0, 0));
    const Vertex s2 = sliver.add_vertex(Point(1, 1e-9, 0));
    const Vertex s3 = sliver.add_vertex(Point(0, 1e-9, 0));
    sliver.add_triangle(s0, s1, s2);
    sliver.add_triangle(s0, s2, s3);
    auto stex = sliver.vertex_property<TexCoord>("v:tex");
    stex[s0] = TexCoord(0, 0);
    stex[s1] = TexCoord(1, 0);
    stex[s2] = TexCoord(1, 1e-9);
    stex[s3] = TexCoord(0, 1e-9);
    auto sres = optimize_uv(sliver, 10, 1e-8);
    check_close(sres.energies.front(), 4.0, 1e-8,
                "thin sliver isometry energy");
    check(all_faces_positive(sliver), "sliver chart must stay positive");
}

// ----------------------------------------------------------------------
// 4. Boundary loops, holes and interior pins stay fixed
// ----------------------------------------------------------------------

void boundary_holes()
{
    // Annulus: outer and inner boundary loops plus an interior pin.
    auto rm = make_ring(4, 14, 4.0, 1.0,
                        [](int, int, double rad, double a) {
                            return Point(rad * std::cos(a),
                                         rad * std::sin(a), 0.0);
                        });
    auto tex = rm.mesh.vertex_property<TexCoord>("v:tex");
    for (int r = 0; r < rm.nr; ++r)
        for (int i = 0; i < rm.nseg; ++i)
        {
            const double a = 2.0 * PI * i / rm.nseg;
            const double rad = 4.0 - r;
            const double jx =
                (r > 0 && r < rm.nr - 1) ? 0.5 * std::sin(4.0 * a) : 0.0;
            tex[rm.v[r][i]] =
                TexCoord(rad * std::cos(a) + jx, rad * std::sin(a));
        }
    auto pinned = rm.mesh.add_vertex_property<bool>("v:uv_fixed");
    const Vertex pin = rm.v[2][3];
    pinned[pin] = true;
    const auto before = snapshot_uv(rm.mesh);
    const double e0 = ref_energy(rm.mesh, tex);

    auto result = optimize_uv(rm.mesh, 100, 1e-9);
    check_result_invariants(result, 100);
    check(result.energies.back() < e0, "annulus interior must improve");
    check_close(result.energies.back(), ref_energy(rm.mesh, tex), 1e-11,
                "annulus final energy matches reference");

    for (auto v : rm.mesh.vertices())
    {
        if (rm.mesh.is_boundary(v) || v == pin)
            for (int c = 0; c < 2; ++c)
                check(tex[v][c] == before[v.idx()][c],
                      "boundary or pin moved in annulus");
    }
    bool interior_moved = false;
    for (int r = 1; r < rm.nr - 1; ++r)
        for (int i = 0; i < rm.nseg; ++i)
        {
            const Vertex v = rm.v[r][i];
            if (v == pin)
                continue;
            if (pmp::norm(tex[v] - before[v.idx()]) > 1e-12)
                interior_moved = true;
        }
    check(interior_moved, "free interior vertices should actually move");
    check(all_faces_positive(rm.mesh), "annulus flipped");

    // Planar grid with two interior holes (deleted faces, no GC).
    SurfaceMesh grid;
    const int nx = 8;
    const int ny = 6;
    std::vector<std::vector<Vertex>> gv(ny,
                                        std::vector<Vertex>(nx));
    for (int y = 0; y < ny; ++y)
        for (int x = 0; x < nx; ++x)
            gv[y][x] = grid.add_vertex(Point(x, y, 0));
    std::vector<Face> faces;
    for (int y = 0; y + 1 < ny; ++y)
        for (int x = 0; x + 1 < nx; ++x)
        {
            if ((x + y) % 2 == 0)
            {
                faces.push_back(
                    grid.add_triangle(gv[y][x], gv[y][x + 1], gv[y + 1][x + 1]));
                faces.push_back(
                    grid.add_triangle(gv[y][x], gv[y + 1][x + 1], gv[y + 1][x]));
            }
            else
            {
                faces.push_back(
                    grid.add_triangle(gv[y][x], gv[y][x + 1], gv[y + 1][x]));
                faces.push_back(
                    grid.add_triangle(gv[y][x + 1], gv[y + 1][x + 1],
                                      gv[y + 1][x]));
            }
        }
    auto gtex = grid.vertex_property<TexCoord>("v:tex");
    for (int y = 0; y < ny; ++y)
        for (int x = 0; x < nx; ++x)
        {
            const double jx = (x > 0 && x < nx - 1 && y > 0 && y < ny - 1)
                                  ? 0.25 * std::sin(2.0 * x + y)
                                  : 0.0;
            gtex[gv[y][x]] = TexCoord(x + jx, y);
        }
    // Punch two well-separated holes; keep the deletion holes around.
    grid.delete_face(faces[10]);
    grid.delete_face(faces[faces.size() - 7]);
    check(grid.faces_size() != grid.n_faces(), "test relies on an uncollected deletion hole");

    std::set<int> boundary_idx;
    for (auto v : grid.vertices())
        if (grid.is_boundary(v))
            boundary_idx.insert(v.idx());
    check(boundary_idx.size() >= 8, "two holes add boundary vertices");
    const auto gbefore = snapshot_uv(grid);
    const double ge0 = ref_energy(grid, gtex);
    auto gres = optimize_uv(grid, 100, 1e-9);
    check_result_invariants(gres, 100);
    check(gres.energies.back() < ge0, "double-hole interior must improve");
    for (int idx : boundary_idx)
        for (int c = 0; c < 2; ++c)
            check(gtex[Vertex(idx)][c] == gbefore[idx][c],
                  "hole boundary moved");
    check(all_faces_positive(grid), "double-hole chart flipped");
}

// ----------------------------------------------------------------------
// 5. Sparse components, isolated vertices, deletion holes, all-fixed mesh
// ----------------------------------------------------------------------

void sparse_components()
{
    SurfaceMesh mesh;

    // Component A: larger annulus.
    auto ra = make_ring(3, 10, 5.0, 1.0,
                        [](int, int, double rad, double a) {
                            return Point(rad * std::cos(a),
                                         rad * std::sin(a), 0.0);
                        });
    std::vector<Vertex> a_verts;
    for (auto v : ra.mesh.vertices())
    {
        const auto w = mesh.add_vertex(ra.mesh.position(v));
        a_verts.push_back(w);
    }
    for (auto f : ra.mesh.faces())
    {
        std::vector<Vertex> fv;
        for (auto v : ra.mesh.vertices(f))
            fv.push_back(a_verts[v.idx()]);
        mesh.add_triangle(fv[0], fv[1], fv[2]);
    }

    // Component B: one small triangle, all boundary => fully fixed,
    // with a much smaller area than component A.
    const Vertex t0 = mesh.add_vertex(Point(10, 0, 0));
    const Vertex t1 = mesh.add_vertex(Point(10.3, 0, 0));
    const Vertex t2 = mesh.add_vertex(Point(10, 0.2, 0));
    mesh.add_triangle(t0, t1, t2);

    // Isolated vertex.
    const Vertex iso = mesh.add_vertex(Point(20, 20, 20));

    auto tex = mesh.vertex_property<TexCoord>("v:tex");
    for (auto v : mesh.vertices())
    {
        const Point p = mesh.position(v);
        tex[v] = TexCoord(p[0], p[1]);
    }
    // Distort the annulus interior only.
    for (int r = 1; r < 2; ++r)
        for (int i = 0; i < 10; ++i)
            tex[a_verts[ra.v[r][i].idx()]] =
                tex[a_verts[ra.v[r][i].idx()]] + TexCoord(0.4, -0.3);

    // Deletion hole inside component A.
    Face victim;
    for (auto f : mesh.faces())
    {
        victim = f;
        break;
    }
    const size_t faces_before = mesh.n_faces();
    mesh.delete_face(victim);
    check(mesh.n_faces() == faces_before - 1, "face deletion accounting");

    // Custom user property that must survive untouched.
    auto custom = mesh.add_vertex_property<int>("v:custom");
    auto points = mesh.get_vertex_property<Point>("v:point");
    std::vector<Point> pos_before(mesh.vertices_size());
    std::vector<int> custom_before(mesh.vertices_size());
    for (auto v : mesh.vertices())
    {
        custom[v] = 1000 + static_cast<int>(v.idx());
        pos_before[v.idx()] = points[v];
        custom_before[v.idx()] = custom[v];
    }
    const auto props_before = mesh.vertex_properties();
    const auto uv_before = snapshot_uv(mesh);
    const double ref_e_before = ref_energy(mesh, tex);

    auto result = optimize_uv(mesh, 100, 1e-9);
    check_result_invariants(result, 100);

    // Reported energies use one total-area-normalized objective across
    // both components, matching the independent reference exactly.
    check_close(result.energies.front(), ref_e_before, 1e-11,
                "multi-component initial energy");
    check_close(result.energies.back(), ref_energy(mesh, tex), 1e-11,
                "multi-component final energy");

    // No temporary properties, and v:uv_fixed was never created.
    check(mesh.vertex_properties() == props_before,
          "vertex property set changed");
    check(!mesh.has_vertex_property("v:uv_fixed"),
          "must not create the fixed property");
    check(mesh.n_faces() == faces_before - 1, "topology changed");
    check(mesh.faces_size() != mesh.n_faces(), "deletion hole must be preserved");
    for (auto v : mesh.vertices())
    {
        check(points[v] == pos_before[v.idx()], "position changed");
        check(custom[v] == custom_before[v.idx()], "custom property changed");
    }
    check(tex[iso] == uv_before[iso.idx()], "isolated vertex UV changed");
    // Fully fixed small triangle keeps its UVs exactly.
    for (auto v : {t0, t1, t2})
        check(tex[v] == uv_before[v.idx()],
              "all-fixed component was modified");
    check(all_faces_positive(mesh), "sparse mesh flipped");

    // A completely fixed mesh: zero steps, zero gradient, one energy entry.
    SurfaceMesh fixed_only;
    const Vertex q0 = fixed_only.add_vertex(Point(0, 0, 0));
    const Vertex q1 = fixed_only.add_vertex(Point(1, 0, 0));
    const Vertex q2 = fixed_only.add_vertex(Point(0, 1, 0));
    fixed_only.add_triangle(q0, q1, q2);
    auto qtex = fixed_only.vertex_property<TexCoord>("v:tex");
    qtex[q0] = TexCoord(0, 0);
    qtex[q1] = TexCoord(1, 0);
    qtex[q2] = TexCoord(0, 1);
    auto qres = optimize_uv(fixed_only, 10, 1e-8);
    check(qres.accepted_steps == 0, "fixed mesh takes no steps");
    check(qres.energies.size() == 1, "fixed mesh records one energy");
    check(qres.gradient_norm == 0.0, "fixed mesh gradient is zero");
    check(qres.converged, "fixed mesh is converged");
}

// ----------------------------------------------------------------------
// 6. Input rejection, rollback and recovery with a subsequent legal call
// ----------------------------------------------------------------------

void rejection()
{
    auto expect_invalid = [](const std::function<void()>& fn,
                             const std::string& what) {
        bool threw = false;
        try
        {
            fn();
        }
        catch (const InvalidInputException&)
        {
            threw = true;
        }
        check(threw, "expected InvalidInputException: " + what);
    };

    // (a) missing v:tex
    {
        SurfaceMesh m;
        auto v0 = m.add_vertex(Point(0, 0, 0));
        auto v1 = m.add_vertex(Point(1, 0, 0));
        auto v2 = m.add_vertex(Point(0, 1, 0));
        m.add_triangle(v0, v1, v2);
        expect_invalid([&] { optimize_uv(m); }, "missing tex property");
    }

    // Build a legal baseline mesh used by the remaining rejection cases.
    auto make_valid = [] {
        auto rm = make_ring(3, 8, 3.0, 1.0,
                            [](int, int, double rad, double a) {
                                return Point(rad * std::cos(a),
                                             rad * std::sin(a), 0.0);
                            });
        auto t = rm.mesh.vertex_property<TexCoord>("v:tex");
        for (int r = 0; r < rm.nr; ++r)
            for (int i = 0; i < rm.nseg; ++i)
            {
                const double a = 2.0 * PI * i / rm.nseg;
                const double rad = 3.0 - r;
                t[rm.v[r][i]] =
                    TexCoord(rad * std::cos(a), rad * std::sin(a));
            }
        return rm;
    };

    // (b) non-finite UV
    {
        auto rm = make_valid();
        auto tex = rm.mesh.get_vertex_property<TexCoord>("v:tex");
        tex[rm.v[1][1]] = TexCoord(std::nan(""), 0.0);
        const auto before = snapshot_uv(rm.mesh);
        expect_invalid([&] { optimize_uv(rm.mesh); }, "non-finite UV");
        check_uv_unchanged(rm.mesh, before, "non-finite UV rollback");
    }

    // (c) non-finite position
    {
        auto rm = make_valid();
        auto points = rm.mesh.get_vertex_property<Point>("v:point");
        points[rm.v[0][0]] = Point(0.0, std::nan(""), 0.0);
        const auto before = snapshot_uv(rm.mesh);
        expect_invalid([&] { optimize_uv(rm.mesh); }, "non-finite point");
        check_uv_unchanged(rm.mesh, before, "non-finite point rollback");
    }

    // (d) non-triangle face
    {
        SurfaceMesh m;
        auto w0 = m.add_vertex(Point(0, 0, 0));
        auto w1 = m.add_vertex(Point(1, 0, 0));
        auto w2 = m.add_vertex(Point(1, 1, 0));
        auto w3 = m.add_vertex(Point(0, 1, 0));
        m.add_quad(w0, w1, w2, w3);
        m.vertex_property<TexCoord>("v:tex");
        const auto before = snapshot_uv(m);
        expect_invalid([&] { optimize_uv(m); }, "quad face");
        check_uv_unchanged(m, before, "quad rollback");
    }

    // (e) zero and negative initial determinant
    {
        auto rm0 = make_valid();
        auto t0 = rm0.mesh.get_vertex_property<TexCoord>("v:tex");
        t0[rm0.v[1][1]] = t0[rm0.v[0][0]]; // collapse a free vertex
        const auto b0 = snapshot_uv(rm0.mesh);
        expect_invalid([&] { optimize_uv(rm0.mesh); }, "zero determinant");
        check_uv_unchanged(rm0.mesh, b0, "zero-det rollback");

        auto rm1 = make_valid();
        auto t1 = rm1.mesh.get_vertex_property<TexCoord>("v:tex");
        // Mirror one interior vertex across an edge to force a flip.
        t1[rm1.v[1][1]] =
            TexCoord(-t1[rm1.v[1][1]][0], -t1[rm1.v[1][1]][1]);
        const auto b1 = snapshot_uv(rm1.mesh);
        expect_invalid([&] { optimize_uv(rm1.mesh); },
                       "negative determinant");
        check_uv_unchanged(rm1.mesh, b1, "negative-det rollback");
    }

    // (f) max_iterations == 0
    {
        auto rm = make_valid();
        const auto before = snapshot_uv(rm.mesh);
        expect_invalid([&] { optimize_uv(rm.mesh, 0); },
                       "zero max_iterations");
        check_uv_unchanged(rm.mesh, before, "max_iter rollback");
    }

    // (g) invalid tolerance values
    {
        auto rm = make_valid();
        const auto before = snapshot_uv(rm.mesh);
        expect_invalid([&] { optimize_uv(rm.mesh, 10, 0.0); },
                       "zero tolerance");
        expect_invalid([&] { optimize_uv(rm.mesh, 10, -1.0); },
                       "negative tolerance");
        expect_invalid(
            [&] {
                optimize_uv(
                    rm.mesh, 10,
                    std::numeric_limits<double>::infinity());
            },
            "infinite tolerance");
        expect_invalid([&] { optimize_uv(rm.mesh, 10, std::nan("")); },
                       "NaN tolerance");
        check_uv_unchanged(rm.mesh, before, "tolerance rollback");
    }

    // (h) closed component without a pinned boundary (no positive chart
    // can exist, so it must be rejected before any UV is touched).
    {
        SurfaceMesh tet;
        auto s0 = tet.add_vertex(Point(0, 0, 0));
        auto s1 = tet.add_vertex(Point(1, 0, 0));
        auto s2 = tet.add_vertex(Point(0, 1, 0));
        auto s3 = tet.add_vertex(Point(0, 0, 1));
        tet.add_triangle(s0, s2, s1);
        tet.add_triangle(s0, s1, s3);
        tet.add_triangle(s1, s2, s3);
        tet.add_triangle(s0, s3, s2);
        auto t = tet.vertex_property<TexCoord>("v:tex");
        t[s0] = TexCoord(0, 0);
        t[s1] = TexCoord(1, 0);
        t[s2] = TexCoord(0, 1);
        t[s3] = TexCoord(0.5, 0.5);
        const auto before = snapshot_uv(tet);
        expect_invalid([&] { optimize_uv(tet); }, "closed component");
        check_uv_unchanged(tet, before, "closed component rollback");
    }

    // (i) zero-area 3D triangle.
    {
        SurfaceMesh m;
        auto z0 = m.add_vertex(Point(0, 0, 0));
        auto z1 = m.add_vertex(Point(1, 0, 0));
        auto z2 = m.add_vertex(Point(2, 0, 0)); // collinear
        m.add_triangle(z0, z1, z2);
        auto t = m.vertex_property<TexCoord>("v:tex");
        t[z0] = TexCoord(0, 0);
        t[z1] = TexCoord(1, 0);
        t[z2] = TexCoord(2, 0);
        const auto before = snapshot_uv(m);
        expect_invalid([&] { optimize_uv(m); }, "zero-area triangle");
        check_uv_unchanged(m, before, "zero-area rollback");
    }

    // Recovery: a legal call on a fresh valid mesh still works.
    auto rm = make_valid();
    auto legal = optimize_uv(rm.mesh, 50, 1e-9);
    check_result_invariants(legal, 50);
    check(all_faces_positive(rm.mesh),
          "legal call after rejections must succeed");

    // A "v:uv_fixed" property of the wrong type is ignored, never replaced
    // or recreated, and boundary pins alone keep the call legal.
    auto wrong = make_valid();
    auto wrong_prop = wrong.mesh.add_vertex_property<int>("v:uv_fixed");
    for (auto v : wrong.mesh.vertices())
        wrong_prop[v] = static_cast<int>(v.idx());
    const auto wrong_before = snapshot_uv(wrong.mesh);
    auto wrong_res = optimize_uv(wrong.mesh, 50, 1e-9);
    check_result_invariants(wrong_res, 50);
    check(wrong.mesh.has_vertex_property("v:uv_fixed"),
          "wrong-typed fixed property must be preserved");
    auto wrong_after =
        wrong.mesh.get_vertex_property<int>("v:uv_fixed");
    check(static_cast<bool>(wrong_after),
          "fixed property must keep its int type");
    for (auto v : wrong.mesh.vertices())
        check(wrong_after[v] == static_cast<int>(v.idx()),
              "wrong-typed fixed property values changed");
    // Boundary UVs still fixed.
    auto wtex = wrong.mesh.get_vertex_property<TexCoord>("v:tex");
    for (int i = 0; i < wrong.nseg; ++i)
        for (int c = 0; c < 2; ++c)
            check(wtex[wrong.v[0][i]][c] ==
                      wrong_before[wrong.v[0][i].idx()][c],
                  "boundary moved with wrong-typed fixed property");
}

} // namespace uv_tests
