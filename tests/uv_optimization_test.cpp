// Copyright 2026 the Polygon Mesh Processing Library developers.
// SPDX-License-Identifier: MIT

// Target tests for pmp::optimize_uv. These functions are declared in
// checks/target_main.cpp and throw on failure; they do not use GoogleTest
// and the reference energy/Jacobian below are independent of the
// production implementation.

#include "pmp/algorithms/uv_optimization.h"

#include <Eigen/Dense>

#include <cmath>
#include <functional>
#include <limits>
#include <numbers>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace uv_tests {

using pmp::Face;
using pmp::InvalidInputException;
using pmp::Point;
using pmp::SurfaceMesh;
using pmp::TexCoord;
using pmp::Vertex;

namespace {

constexpr double PI = std::numbers::pi;

void check(bool condition, const std::string& message)
{
    if (!condition)
        throw std::runtime_error("uv test failure: " + message);
}

void require_throws(const std::function<void()>& call,
                    const std::string& label)
{
    bool threw = false;
    try
    {
        call();
    }
    catch (const InvalidInputException&)
    {
        threw = true;
    }
    catch (const pmp::SolverException&)
    {
        threw = true;
    }
    check(threw, "expected exception for: " + label);
}

// Independent per-face reference Jacobian following the task definition,
// without calling any production energy code.
struct RefFace
{
    Vertex v[3];
    double area;
    Eigen::Matrix2d dinv;
};

std::vector<RefFace> reference_faces(const SurfaceMesh& mesh)
{
    std::vector<RefFace> result;
    for (auto f : mesh.faces())
    {
        auto it = mesh.vertices(f);
        const Vertex v0 = *it;
        ++it;
        const Vertex v1 = *it;
        ++it;
        const Vertex v2 = *it;
        const Point p0 = mesh.position(v0);
        const Point p1 = mesh.position(v1);
        const Point p2 = mesh.position(v2);

        const Point e0 = p1 - p0;
        const double length = pmp::norm(e0);
        const Point e1 = e0 / length;
        const Point pp = p2 - p0;
        const double q2x = pmp::dot(pp, e1);
        const Point perp = pp - q2x * e1;
        const double q2y = pmp::norm(perp);

        RefFace rf;
        rf.v[0] = v0;
        rf.v[1] = v1;
        rf.v[2] = v2;
        rf.area = 0.5 * length * q2y;
        Eigen::Matrix2d d;
        d << length, q2x, 0.0, q2y;
        rf.dinv = d.inverse();
        result.push_back(rf);
    }
    return result;
}

Eigen::Matrix2d reference_jacobian(
    const RefFace& rf,
    const pmp::VertexProperty<TexCoord>& tex)
{
    const TexCoord& u0 = tex[rf.v[0]];
    const TexCoord& u1 = tex[rf.v[1]];
    const TexCoord& u2 = tex[rf.v[2]];
    Eigen::Matrix2d u;
    u << u1[0] - u0[0], u2[0] - u0[0],
        u1[1] - u0[1], u2[1] - u0[1];
    return u * rf.dinv;
}

double reference_energy(
    const std::vector<RefFace>& faces,
    const pmp::VertexProperty<TexCoord>& tex)
{
    double total_area = 0.0;
    double sum = 0.0;
    for (const auto& rf : faces)
    {
        const Eigen::Matrix2d j = reference_jacobian(rf, tex);
        const double det = j.determinant();
        const double fn2 = j.squaredNorm();
        sum += rf.area * (fn2 + fn2 / (det * det));
        total_area += rf.area;
    }
    return sum / total_area;
}

double reference_energy(const SurfaceMesh& mesh)
{
    auto tex = mesh.get_vertex_property<TexCoord>("v:tex");
    auto faces = reference_faces(mesh);
    return reference_energy(faces, tex);
}

// Annular triangulation with NL loops, NS segments.
struct Annulus
{
    SurfaceMesh mesh;
    std::vector<std::vector<Vertex>> v;
};

Annulus make_annulus(int nl, int ns,
                     const std::function<double(int, int)>& radius,
                     const std::function<double(int, int)>& height)
{
    Annulus a;
    a.v.assign(nl, std::vector<Vertex>(ns));
    for (int r = 0; r < nl; ++r)
        for (int i = 0; i < ns; ++i)
        {
            const double angle = 2.0 * PI * i / ns;
            const double rad = radius(r, i);
            a.v[r][i] = a.mesh.add_vertex(Point(
                rad * std::cos(angle), rad * std::sin(angle),
                height(r, i)));
        }
    for (int r = 0; r < nl - 1; ++r)
        for (int i = 0; i < ns; ++i)
        {
            const int j = (i + 1) % ns;
            a.mesh.add_triangle(a.v[r][i], a.v[r][j], a.v[r + 1][j]);
            a.mesh.add_triangle(a.v[r][i], a.v[r + 1][j],
                                a.v[r + 1][i]);
        }
    return a;
}

// Distorted but strictly orientation-preserving radial chart: the ring
// radii are squashed together so the interior triangles are stretched.
void set_radial_chart(Annulus& a, const std::vector<double>& ring_radii,
                      double wobble = 0.05)
{
    auto tex = a.mesh.vertex_property<TexCoord>("v:tex");
    const int ns = static_cast<int>(a.v[0].size());
    for (size_t r = 0; r < a.v.size(); ++r)
        for (int i = 0; i < ns; ++i)
        {
            const double angle = 2.0 * PI * i / ns;
            const double rad =
                ring_radii[r] * (1.0 + wobble * std::sin(3.0 * angle));
            tex[a.v[r][i]] =
                TexCoord(rad * std::cos(angle), rad * std::sin(angle));
        }
}

std::vector<TexCoord> snapshot_uv(const SurfaceMesh& mesh)
{
    auto tex = mesh.get_vertex_property<TexCoord>("v:tex");
    std::vector<TexCoord> snap(mesh.vertices_size());
    for (auto v : mesh.vertices())
        snap[v.idx()] = tex[v];
    return snap;
}

// NaN-aware equality: a rejected call must restore the exact bits,
// including a non-finite input coordinate.
bool same_scalar(double a, double b)
{
    if (std::isnan(a))
        return std::isnan(b);
    return a == b;
}

void check_uv_unchanged(const SurfaceMesh& mesh,
                        const std::vector<TexCoord>& snap,
                        const std::string& label)
{
    auto tex = mesh.get_vertex_property<TexCoord>("v:tex");
    for (auto v : mesh.vertices())
        for (int k = 0; k < 2; ++k)
            check(same_scalar(tex[v][k], snap[v.idx()][k]),
                  label + ": UV coordinate changed on a failed call");
}

} // namespace

void energy_gradient()
{
    // Fan of five non-coplanar, unequal-area triangles around one free
    // interior vertex; the five boundary vertices are pinned.
    SurfaceMesh mesh;
    const int ns = 5;
    std::vector<Vertex> ring(ns);
    for (int i = 0; i < ns; ++i)
    {
        const double angle = 2.0 * PI * i / ns;
        const double rad = 2.0 + 0.7 * std::sin(2.0 * angle);
        ring[i] = mesh.add_vertex(Point(
            rad * std::cos(angle), rad * std::sin(angle),
            0.8 * std::sin(3.0 * angle)));
    }
    const Vertex center = mesh.add_vertex(Point(0.1, -0.2, 0.35));
    for (int i = 0; i < ns; ++i)
        mesh.add_triangle(center, ring[i], ring[(i + 1) % ns]);

    auto tex = mesh.vertex_property<TexCoord>("v:tex");
    for (int i = 0; i < ns; ++i)
    {
        const double angle = 2.0 * PI * i / ns;
        tex[ring[i]] = TexCoord(1.6 * std::cos(angle),
                                1.6 * std::sin(angle));
    }
    tex[center] = TexCoord(0.25, -0.15);

    auto ref_faces = reference_faces(mesh);
    double area_min = std::numeric_limits<double>::infinity();
    double area_max = 0.0;
    for (const auto& rf : ref_faces)
    {
        area_min = std::min(area_min, rf.area);
        area_max = std::max(area_max, rf.area);
    }
    check(area_max > 1.5 * area_min,
          "test mesh needs triangles of unequal area");

    const double initial_energy =
        reference_energy(ref_faces, tex);
    check(std::isfinite(initial_energy), "initial energy not finite");

    const auto result = pmp::optimize_uv(mesh, 100, 1e-11);
    check(result.energies.size() == result.accepted_steps + 1,
          "energies must contain input plus accepted states");
    check(result.energies.front() == initial_energy ||
              std::abs(result.energies.front() - initial_energy) < 1e-12,
          "first reported energy must match the independent reference");

    // Every reference face keeps a positive determinant.
    for (const auto& rf : ref_faces)
    {
        const Eigen::Matrix2d j = reference_jacobian(rf, tex);
        check(j.determinant() > 0.0, "flipped triangle after optimization");
    }

    // Independent final energy matches the reported value.
    const double final_energy = reference_energy(ref_faces, tex);
    check(std::abs(final_energy - result.energies.back()) <=
              1e-10 * std::max(1.0, final_energy),
          "reported energy disagrees with the independent reference");

    // Central-difference gradient at the final state versus the analytic
    // diagnostic and the reported infinity norm.
    const auto analytic = pmp::detail::uv_objective_gradient(mesh);
    const double h = 1e-6;
    double cd_norm = 0.0;
    double max_error = 0.0;
    for (auto v : mesh.vertices())
    {
        if (mesh.is_boundary(v))
            continue;
        for (int k = 0; k < 2; ++k)
        {
            const double saved = tex[v][k];
            tex[v][k] = saved + h;
            const double e_plus =
                reference_energy(ref_faces, tex);
            tex[v][k] = saved - h;
            const double e_minus =
                reference_energy(ref_faces, tex);
            tex[v][k] = saved;
            const double fd = (e_plus - e_minus) / (2.0 * h);
            const double exact = analytic[2 * v.idx() + k];
            max_error = std::max(max_error, std::abs(fd - exact));
            cd_norm = std::max(cd_norm, std::abs(fd));
        }
    }
    check(max_error <= 1e-7,
          "analytic gradient does not match central differences");
    check(std::abs(cd_norm - result.gradient_norm) <= 1e-7,
          "reported gradient norm does not match central differences");
}

void distortion_reduction()
{
    // Asymmetric annulus with two free interior loops (NL = 4).
    const int nl = 4;
    const int ns = 18;
    Annulus a = make_annulus(
        nl, ns,
        [](int r, int i) {
            const double angle = 2.0 * PI * i / ns;
            return 3.2 - 0.75 * r +
                   0.18 * std::sin(3.0 * angle) * (r + 1);
        },
        [](int r, int i) {
            const double angle = 2.0 * PI * i / ns;
            return 0.45 * std::sin(2.0 * angle) * (r == 1 ? 1.0 : 0.3) +
                   0.25 * r;
        });
    const std::vector<double> distorted_radii{2.6, 1.15, 0.8, 0.42};
    set_radial_chart(a, distorted_radii);

    auto tex = a.mesh.vertex_property<TexCoord>("v:tex");
    const double energy_before = reference_energy(a.mesh);
    const auto result = pmp::optimize_uv(a.mesh, 200, 1e-10);
    check(result.accepted_steps <= 200, "too many accepted steps");
    const double energy_after = result.energies.back();
    check(energy_after <= 0.95 * energy_before,
          "energy did not drop by at least 5%");
    check(std::abs(energy_after - reference_energy(a.mesh)) < 1e-10,
          "reported energy mismatch");
    for (auto f : a.mesh.faces())
    {
        auto it = a.mesh.vertices(f);
        Vertex v0 = *it;
        ++it;
        Vertex v1 = *it;
        ++it;
        Vertex v2 = *it;
        Eigen::Matrix2d u;
        u << tex[v1][0] - tex[v0][0], tex[v2][0] - tex[v0][0],
            tex[v1][1] - tex[v0][1], tex[v2][1] - tex[v0][1];
        check(u.determinant() > 0.0, "flip in distorted annulus");
    }

    // Regular planar annulus with both boundary loops pinned to the true
    // planar coordinates: the unique minimum is the identity map, J = I,
    // energy = 4 on every face.
    Annulus planar = make_annulus(
        3, 24,
        [](int r, int) { return 3.0 - r; },
        [](int, int) { return 0.0; });
    auto ptex = planar.mesh.vertex_property<TexCoord>("v:tex");
    // Start from a visibly distorted chart.
    const std::vector<double> start_radii{3.0, 1.4, 1.0};
    set_radial_chart(planar, start_radii, 0.08);
    // Pin both boundary loops to the exact planar reference positions.
    for (int i = 0; i < 24; ++i)
    {
        const double angle = 2.0 * PI * i / 24;
        ptex[planar.v[0][i]] =
            TexCoord(3.0 * std::cos(angle), 3.0 * std::sin(angle));
        ptex[planar.v[2][i]] =
            TexCoord(1.0 * std::cos(angle), 1.0 * std::sin(angle));
    }
    const auto presult = pmp::optimize_uv(planar.mesh, 500, 1e-8);
    check(presult.converged, "planar reference case did not converge");
    check(std::abs(presult.energies.back() - 4.0) <= 1e-7,
          "planar identity optimum must have energy 4");
    for (int i = 0; i < 24; ++i)
    {
        const Point& p = planar.mesh.position(planar.v[1][i]);
        const TexCoord& u = ptex[planar.v[1][i]];
        check(std::abs(u[0] - p[0]) <= 1e-7 &&
                  std::abs(u[1] - p[1]) <= 1e-7,
              "interior did not converge to the known planar solution");
    }
}

void no_flip()
{
    // Sliver triangles: a thin 3D fan around one free interior vertex,
    // starting from a chart whose determinants are positive but tiny.
    SurfaceMesh mesh;
    const int ns = 6;
    std::vector<Vertex> ring(ns);
    for (int i = 0; i < ns; ++i)
    {
        const double angle = 2.0 * PI * i / ns;
        ring[i] = mesh.add_vertex(Point(
            5.0 * std::cos(angle), 5.0 * std::sin(angle),
            1e-5 * std::sin(3.0 * angle)));
    }
    const Vertex center =
        mesh.add_vertex(Point(1e-7, -2e-7, 0.2));
    for (int i = 0; i < ns; ++i)
        mesh.add_triangle(center, ring[i], ring[(i + 1) % ns]);

    auto tex = mesh.vertex_property<TexCoord>("v:tex");
    for (int i = 0; i < ns; ++i)
    {
        const double angle = 2.0 * PI * i / ns;
        tex[ring[i]] =
            TexCoord(5.0 * std::cos(angle), 5.0 * std::sin(angle));
    }
    // The interior vertex sits almost on an edge, so the two adjacent
    // reference triangles start with a very small positive determinant.
    tex[center] = TexCoord(4.95, 1e-4);

    const auto ref_faces = reference_faces(mesh);
    double min_det = std::numeric_limits<double>::infinity();
    for (const auto& rf : ref_faces)
    {
        const double det =
            reference_jacobian(rf, tex).determinant();
        check(det > 0.0,
              "precondition: all initial determinants positive");
        min_det = std::min(min_det, det);
    }
    check(min_det < 0.05, "test needs a genuinely near-flipped start");

    const auto result = pmp::optimize_uv(mesh, 100, 1e-10);
    for (const auto& rf : ref_faces)
    {
        const double det =
            reference_jacobian(rf, tex).determinant();
        check(det > 0.0, "sliver triangle flipped during optimization");
    }
    for (double e : result.energies)
        check(std::isfinite(e), "non-finite energy near a flip");

    // Independent direction whose determinant is positive at t = 0 and
    // t = 4 but negative in between: p(t) = 1 - 4t + t^2, roots at
    // 2 - sqrt(3) and 2 + sqrt(3).
    const double s = std::sqrt(3.0);
    const std::array<double, 4> J{1.0, 0.0, 0.0, 1.0};
    const std::array<double, 4> dJ{-2.0 + s, 0.0, 0.0, -2.0 - s};
    auto poly = [](double t) { return 1.0 - 4.0 * t + t * t; };
    check(poly(0.0) > 0.0 && poly(4.0) > 0.0 && poly(1.0) < 0.0,
          "test direction must be positive at both ends, negative inside");
    const double limit = pmp::detail::uv_positive_step_limit(J, dJ);
    const double first_root = 2.0 - s;
    check(std::abs(limit - first_root) <= 1e-12,
          "step limit must stop at the first determinant zero");
    check(limit < 4.0, "endpoint-only check would miss the internal zero");
    check(std::abs(poly(0.99 * limit)) > 0.0 &&
              poly(0.99 * limit) > 0.0,
          "safety-margin step must stay strictly positive");

    // Linear, repeated-root and root-free cases.
    const std::array<double, 4> identity{1.0, 0.0, 0.0, 1.0};
    check(std::abs(pmp::detail::uv_positive_step_limit(
                       identity, std::array<double, 4>{-1.0, 0.0, 0.0, 0.0}) -
                   1.0) <= 1e-12,
          "linear root");
    check(std::abs(pmp::detail::uv_positive_step_limit(
                       identity,
                       std::array<double, 4>{-0.5, 0.0, 0.0, -0.5}) -
                   2.0) <= 1e-12,
          "repeated root");
    check(std::isinf(pmp::detail::uv_positive_step_limit(
              identity, std::array<double, 4>{1.0, 0.0, 0.0, 1.0})),
          "root-free direction must return infinity");
}

void boundary_holes()
{
    // Single-hole annulus with an interior pin.
    Annulus a = make_annulus(
        4, 16, [](int r, int) { return 3.0 - 0.7 * r; },
        [](int r, int i) {
            const double angle = 2.0 * PI * i / 16;
            return 0.3 * std::sin(2.0 * angle) + 0.1 * r;
        });
    const std::vector<double> radii{2.6, 1.2, 0.85, 0.5};
    set_radial_chart(a, radii);
    auto fixed = a.mesh.add_vertex_property<bool>("v:uv_fixed", false);
    const Vertex pin = a.v[2][5];
    fixed[pin] = true;

    const auto before = snapshot_uv(a.mesh);
    const double e_before = reference_energy(a.mesh);
    const auto result = pmp::optimize_uv(a.mesh, 200, 1e-10);
    check(result.energies.back() < e_before,
          "annulus interior did not improve");

    auto tex = a.mesh.vertex_property<TexCoord>("v:tex");
    for (auto v : a.mesh.vertices())
    {
        if (a.mesh.is_boundary(v) || v == pin)
        {
            check(tex[v][0] == before[v.idx()][0] &&
                      tex[v][1] == before[v.idx()][1],
                  "boundary or pinned vertex moved");
        }
    }
    bool interior_moved = false;
    for (auto v : a.mesh.vertices())
    {
        if (!a.mesh.is_boundary(v) && v != pin &&
            (tex[v][0] != before[v.idx()][0] ||
             tex[v][1] != before[v.idx()][1]))
            interior_moved = true;
    }
    check(interior_moved, "no free interior vertex was optimized");
    a.mesh.remove_vertex_property(fixed);

    // Connected planar domain with two holes: a lattice strip with two
    // separated square cells removed.
    SurfaceMesh grid;
    const int n = 8;
    std::vector<std::vector<Vertex>> gv(
        n + 1, std::vector<Vertex>(n + 1));
    for (int i = 0; i <= n; ++i)
        for (int j = 0; j <= n; ++j)
            gv[i][j] = grid.add_vertex(Point(i, j, 0.0));

    auto cell_removed = [](int i, int j) {
        const bool first_hole = (i == 1 || i == 2) &&
                                (j == 1 || j == 2);
        const bool second_hole = (i == 5 || i == 6) &&
                                 (j == 5 || j == 6);
        return first_hole || second_hole;
    };
    for (int i = 0; i < n; ++i)
        for (int j = 0; j < n; ++j)
        {
            if (cell_removed(i, j))
                continue;
            grid.add_triangle(gv[i][j], gv[i + 1][j], gv[i + 1][j + 1]);
            grid.add_triangle(gv[i][j], gv[i + 1][j + 1], gv[i][j + 1]);
        }

    auto gtex = grid.vertex_property<TexCoord>("v:tex");
    for (int i = 0; i <= n; ++i)
        for (int j = 0; j <= n; ++j)
        {
            // Affine boundary data, but a non-affine wobble on interior
            // vertices: the chart stays orientation-preserving but is no
            // longer the affine minimizer, so it must actually improve.
            const double wobble =
                grid.is_boundary(gv[i][j])
                    ? 0.0
                    : 0.25 * std::sin(1.7 * i) * std::cos(1.3 * j);
            gtex[gv[i][j]] =
                TexCoord(0.7 * i + wobble, 1.3 * j - wobble);
        }

    const auto g_before = snapshot_uv(grid);
    const double ge_before = reference_energy(grid);
    const auto gresult = pmp::optimize_uv(grid, 300, 1e-10);
    check(gresult.energies.back() < ge_before,
          "double-hole domain did not improve");
    check(gresult.energies.back() <= 0.99 * ge_before,
          "double-hole improvement is negligible");

    int boundary_count = 0;
    for (auto v : grid.vertices())
        if (grid.is_boundary(v))
            ++boundary_count;
    check(boundary_count > 8, "double-hole mesh lacks boundary loops");
    for (auto v : grid.vertices())
    {
        if (grid.is_boundary(v))
            check(gtex[v][0] == g_before[v.idx()][0] &&
                      gtex[v][1] == g_before[v.idx()][1],
                  "double-hole boundary vertex moved");
    }
}

void sparse_components()
{
    SurfaceMesh mesh;

    // Two disconnected annuli of different scales (unequal face areas).
    Annulus big = make_annulus(
        3, 12, [](int r, int) { return 4.0 - r; },
        [](int, int) { return 0.0; });
    Annulus small = make_annulus(
        3, 10, [](int r, int) { return 0.4 - 0.1 * r; },
        [](int r, int i) {
            const double angle = 2.0 * PI * i / 10;
            return 0.05 * std::cos(angle) + 0.02 * r;
        });

    std::vector<Vertex> big_v, small_v;
    for (auto v : big.mesh.vertices())
        big_v.push_back(mesh.add_vertex(big.mesh.position(v)));
    for (auto v : small.mesh.vertices())
        small_v.push_back(mesh.add_vertex(small.mesh.position(v)));
    for (auto f : big.mesh.faces())
    {
        auto it = big.mesh.vertices(f);
        Vertex a = *it;
        ++it;
        Vertex b = *it;
        ++it;
        Vertex c = *it;
        mesh.add_triangle(big_v[a.idx()], big_v[b.idx()], big_v[c.idx()]);
    }
    for (auto f : small.mesh.faces())
    {
        auto it = small.mesh.vertices(f);
        Vertex a = *it;
        ++it;
        Vertex b = *it;
        ++it;
        Vertex c = *it;
        mesh.add_triangle(small_v[a.idx()], small_v[b.idx()],
                          small_v[c.idx()]);
    }

    // Fully fixed component: one triangle, every vertex is on the
    // boundary and gets distinct UVs.
    const Vertex t0 = mesh.add_vertex(Point(10.0, 0.0, 0.0));
    const Vertex t1 = mesh.add_vertex(Point(11.0, 0.0, 0.0));
    const Vertex t2 = mesh.add_vertex(Point(10.0, 1.0, 0.0));
    mesh.add_triangle(t0, t1, t2);

    // Isolated vertex.
    const Vertex iso = mesh.add_vertex(Point(20.0, 20.0, 20.0));

    auto tex = mesh.vertex_property<TexCoord>("v:tex");
    set_radial_chart(big, {3.2, 1.5, 1.0});
    auto btex = big.mesh.get_vertex_property<TexCoord>("v:tex");
    for (auto v : big.mesh.vertices())
        tex[big_v[v.idx()]] = btex[v];
    set_radial_chart(small, {0.35, 0.2, 0.1});
    auto stex = small.mesh.get_vertex_property<TexCoord>("v:tex");
    for (auto v : small.mesh.vertices())
        tex[small_v[v.idx()]] = stex[v];
    tex[t0] = TexCoord(0.0, 0.0);
    tex[t1] = TexCoord(1.0, 0.0);
    tex[t2] = TexCoord(0.0, 1.0);
    tex[iso] = TexCoord(7.25, -3.5);

    // Deleted face left as garbage without garbage collection.
    const Face victim = *mesh.faces_begin();
    mesh.delete_face(victim);

    // Custom user property that must survive untouched.
    auto custom = mesh.add_vertex_property<double>("v:custom", -1.0);
    for (auto v : mesh.vertices())
        custom[v] = 3.14159 * (v.idx() + 1);
    const auto points_before = mesh.positions();
    const auto props_before = mesh.vertex_properties();
    check(!mesh.has_vertex_property("v:uv_fixed"),
          "fixture must not define v:uv_fixed");

    const auto snap = snapshot_uv(mesh);
    const double e_before = reference_energy(mesh);
    const auto result = pmp::optimize_uv(mesh, 200, 1e-10);
    check(result.energies.back() < e_before,
          "multi-component energy did not improve");

    // Isolated and all-fixed vertices stay exactly in place.
    check(tex[iso][0] == 7.25 && tex[iso][1] == -3.5,
          "isolated vertex moved");
    for (Vertex tv : {t0, t1, t2})
        check(tex[tv][0] == snap[tv.idx()][0] &&
                  tex[tv][1] == snap[tv.idx()][1],
              "fully fixed component moved");

    // Positions, custom property and the property set are untouched.
    check(mesh.positions().size() == points_before.size(),
          "position storage changed");
    for (size_t i = 0; i < points_before.size(); ++i)
        for (int k = 0; k < 3; ++k)
            check(mesh.positions()[i][k] == points_before[i][k],
                  "vertex position changed");
    for (auto v : mesh.vertices())
        check(custom[v] == 3.14159 * (v.idx() + 1),
              "custom property changed");
    check(mesh.vertex_properties() == props_before,
          "temporary properties were left on the mesh");
    check(!mesh.has_vertex_property("v:uv_fixed"),
          "v:uv_fixed must not be created when absent");
    mesh.remove_vertex_property(custom);

    // A mesh with no free variables returns zero steps and zero gradient.
    SurfaceMesh fixed_mesh;
    auto f0 = fixed_mesh.add_vertex(Point(0, 0, 0));
    auto f1 = fixed_mesh.add_vertex(Point(1, 0, 0));
    auto f2 = fixed_mesh.add_vertex(Point(0, 1, 0));
    fixed_mesh.add_triangle(f0, f1, f2);
    auto ftex = fixed_mesh.vertex_property<TexCoord>("v:tex");
    ftex[f0] = TexCoord(0, 0);
    ftex[f1] = TexCoord(1, 0);
    ftex[f2] = TexCoord(0, 1);
    const auto fixed_result = pmp::optimize_uv(fixed_mesh, 10);
    check(fixed_result.accepted_steps == 0,
          "fully fixed mesh should take no steps");
    check(fixed_result.gradient_norm == 0.0,
          "gradient norm must be zero without free variables");
    check(fixed_result.energies.size() == 1 &&
              std::isfinite(fixed_result.energies[0]),
          "fully fixed mesh must still report its energy");
}

void rejection()
{
    auto base_mesh = [] {
        SurfaceMesh m;
        auto a = m.add_vertex(Point(0, 0, 0));
        auto b = m.add_vertex(Point(1, 0, 0));
        auto c = m.add_vertex(Point(0, 1, 0));
        auto d = m.add_vertex(Point(1, 1, 0));
        m.add_triangle(a, b, c);
        m.add_triangle(b, d, c);
        auto tex = m.vertex_property<TexCoord>("v:tex");
        tex[a] = TexCoord(0, 0);
        tex[b] = TexCoord(1, 0);
        tex[c] = TexCoord(0, 1);
        tex[d] = TexCoord(1, 1);
        return m;
    };

    // Non-triangle face.
    {
        SurfaceMesh m = base_mesh();
        auto q0 = m.add_vertex(Point(2, 0, 0));
        auto q1 = m.add_vertex(Point(3, 0, 0));
        auto q2 = m.add_vertex(Point(3, 1, 0));
        auto q3 = m.add_vertex(Point(2, 1, 0));
        m.add_quad(q0, q1, q2, q3);
        auto tex = m.get_vertex_property<TexCoord>("v:tex");
        tex[q0] = TexCoord(2, 0);
        tex[q1] = TexCoord(3, 0);
        tex[q2] = TexCoord(3, 1);
        tex[q3] = TexCoord(2, 1);
        require_throws([&] { pmp::optimize_uv(m); }, "non-triangle face");
    }

    // Missing v:tex.
    {
        SurfaceMesh m;
        m.add_triangle(m.add_vertex(Point(0, 0, 0)),
                       m.add_vertex(Point(1, 0, 0)),
                       m.add_vertex(Point(0, 1, 0)));
        require_throws([&] { pmp::optimize_uv(m); }, "missing v:tex");
    }

    // Non-finite UV.
    {
        SurfaceMesh m = base_mesh();
        auto tex = m.get_vertex_property<TexCoord>("v:tex");
        tex[Vertex(0)] = TexCoord(std::nan(""), 0.0);
        const auto snap = snapshot_uv(m);
        require_throws([&] { pmp::optimize_uv(m); }, "non-finite UV");
        check_uv_unchanged(m, snap, "non-finite UV");
    }

    // Zero and negative initial Jacobian determinant.
    for (int mode = 0; mode < 2; ++mode)
    {
        SurfaceMesh m = base_mesh();
        auto tex = m.get_vertex_property<TexCoord>("v:tex");
        if (mode == 0)
            tex[Vertex(2)] = TexCoord(1.0, 0.0); // degenerate: det == 0
        else
            tex[Vertex(2)] = TexCoord(0.0, -1.0); // reversed orientation
        const auto snap = snapshot_uv(m);
        require_throws([&] { pmp::optimize_uv(m); },
                       mode == 0 ? "zero determinant"
                                 : "negative determinant");
        check_uv_unchanged(m, snap, "bad determinant");
    }

    // Non-finite position.
    {
        SurfaceMesh m = base_mesh();
        m.position(Vertex(0)) = Point(std::nan(""), 0.0, 0.0);
        const auto snap = snapshot_uv(m);
        require_throws([&] { pmp::optimize_uv(m); }, "non-finite position");
        check_uv_unchanged(m, snap, "non-finite position");
    }

    // Bad iteration counts and tolerances.
    {
        SurfaceMesh m = base_mesh();
        require_throws([&] { pmp::optimize_uv(m, 0); },
                       "max_iterations = 0");
        require_throws([&] { pmp::optimize_uv(m, 10, 0.0); },
                       "tolerance = 0");
        require_throws([&] { pmp::optimize_uv(m, 10, -1.0); },
                       "negative tolerance");
        require_throws([&] {
            pmp::optimize_uv(m, 10,
                             std::numeric_limits<double>::infinity());
        },
                       "infinite tolerance");
        require_throws([&] {
            pmp::optimize_uv(m, 10, std::nan(""));
        },
                       "NaN tolerance");
    }

    // Component with fewer than two distinct pinned UV positions: every
    // boundary vertex collapses to the same texture point.
    {
        Annulus a = make_annulus(
            3, 10, [](int r, int) { return 2.0 - 0.5 * r; },
            [](int, int) { return 0.0; });
        auto tex = a.mesh.vertex_property<TexCoord>("v:tex");
        for (auto v : a.mesh.vertices())
            tex[v] = TexCoord(0.5, 0.5);
        for (int i = 0; i < 10; ++i)
        {
            tex[a.v[1][i]] =
                TexCoord(0.5 + 0.05 * std::cos(2.0 * PI * i / 10),
                         0.5 + 0.05 * std::sin(2.0 * PI * i / 10));
        }
        const auto snap = snapshot_uv(a.mesh);
        require_throws([&] { pmp::optimize_uv(a.mesh); },
                       "single pinned UV position");
        check_uv_unchanged(a.mesh, snap, "single pinned UV position");
    }

    // After all rejections, a legal call still works.
    {
        SurfaceMesh m = base_mesh();
        const auto snap = snapshot_uv(m);
        const auto result = pmp::optimize_uv(m, 50);
        check(result.energies.size() == result.accepted_steps + 1,
              "recovery call returned a malformed result");
        check(result.energies.front() > 0.0 &&
              std::isfinite(result.energies.back()),
              "recovery call returned a bad energy");
        auto tex = m.get_vertex_property<TexCoord>("v:tex");
        for (auto v : m.vertices())
            check(tex[v][0] == snap[v.idx()][0] &&
                      tex[v][1] == snap[v.idx()][1],
                  "boundary UVs moved on the legal recovery call");
    }
}

} // namespace uv_tests
