/**
 * Regression tests for tinynurbs::tessellatePatches.
 *
 * These tests exercise the sampled chord criterion, coverage of every
 * non-zero knot interval, adaptive local density, seam welding in both
 * directions, corner identity, and rejection of malformed input.
 *
 * Use of this source code is governed by a BSD-style license that can be found in
 * the LICENSE file.
 */

#include <tinynurbs/tinynurbs.h>

#include <glm/glm.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <map>
#include <set>
#include <tuple>
#include <utility>
#include <vector>

#include "catch.hpp"

namespace
{

using tinynurbs::array2;
using tinynurbs::PatchEdge;
using tinynurbs::PatchSeam;
using tinynurbs::RationalSurface3d;
using tinynurbs::TessellationMesh;
using tinynurbs::TessellationOptions;
using tinynurbs::tessellatePatches;

typedef std::pair<double, double> Domain;

RationalSurface3d makeUnitPlane(double z = 0.0)
{
    RationalSurface3d s;
    s.degree_u = 1;
    s.degree_v = 1;
    s.knots_u = {0, 0, 1, 1};
    s.knots_v = {0, 0, 1, 1};
    s.control_points = {2, 2, {glm::dvec3(0, 0, z), glm::dvec3(0, 1, z), glm::dvec3(1, 0, z),
                               glm::dvec3(1, 1, z)}};
    s.weights = array2<double>(2, 2, 1.0);
    return s;
}

// The 3x3 control net from the task description: flat except for the
// upper-right corner control point, split by knots at 0.5 in both directions.
RationalSurface3d makeBump()
{
    RationalSurface3d s;
    s.degree_u = 1;
    s.degree_v = 1;
    s.knots_u = {0, 0, .5, 1, 1};
    s.knots_v = {0, 0, .5, 1, 1};
    s.control_points.resize(3, 3);
    s.weights = array2<double>(3, 3, 1.0);
    const double coord[3] = {0.0, .5, 1.0};
    for (size_t i = 0; i < 3; ++i)
        for (size_t j = 0; j < 3; ++j)
            s.control_points(i, j) =
                glm::dvec3(coord[i], coord[j], (i == 2 && j == 2) ? 1.0 : 0.0);
    return s;
}

// Quarter-cylinder band.  The second band mirrors the geometry in u (its
// UMin edge is the first band's UMax edge) and runs backwards in v.
RationalSurface3d makeBand(bool second)
{
    RationalSurface3d s;
    s.degree_u = 1;
    s.degree_v = 2;
    s.knots_u = second ? std::vector<double>{-4, -4, 8, 8} : std::vector<double>{2, 2, 5, 5};
    s.knots_v = second ? std::vector<double>{10, 10, 10, 14, 14, 14}
                       : std::vector<double>{-3, -3, -3, 7, 7, 7};
    s.control_points.resize(2, 3);
    s.weights.resize(2, 3);
    const glm::dvec3 arc[] = {glm::dvec3(0, 1, 0), glm::dvec3(0, 1, 1), glm::dvec3(0, 0, 1)};
    for (size_t i = 0; i < 2; ++i)
        for (size_t j = 0; j < 3; ++j)
        {
            s.control_points(i, j) =
                arc[second ? 2 - j : j] + glm::dvec3(double(i) + (second ? 1.0 : 0.0), 0, 0);
            s.weights(i, j) = j == 1 ? std::sqrt(0.5) : 1.0;
        }
    return s;
}

RationalSurface3d makeQuad(glm::dvec3 origin, glm::dvec3 du, glm::dvec3 dv)
{
    RationalSurface3d s;
    s.degree_u = 1;
    s.degree_v = 1;
    s.knots_u = {0, 0, 1, 1};
    s.knots_v = {0, 0, 1, 1};
    s.control_points = {2, 2, {origin, origin + dv, origin + du, origin + du + dv}};
    s.weights = array2<double>(2, 2, 1.0);
    return s;
}

std::vector<Domain> roots(const std::vector<double> &knots, unsigned int degree,
                          size_t num_control_points)
{
    std::vector<Domain> out;
    for (size_t k = degree; k < num_control_points; ++k)
        if (knots[k + 1] > knots[k])
            out.push_back(std::make_pair(knots[k], knots[k + 1]));
    return out;
}

std::pair<size_t, size_t> rootsOf(const RationalSurface3d &s)
{
    return std::make_pair(roots(s.knots_u, s.degree_u, s.control_points.rows()).size(),
                          roots(s.knots_v, s.degree_v, s.control_points.cols()).size());
}

glm::dvec3 surfaceAt(const RationalSurface3d &s, glm::dvec2 uv)
{
    return tinynurbs::surfacePoint(s, uv.x, uv.y);
}

// Index of the non-zero knot interval containing x, or npos when x lies
// outside the valid domain.
size_t intervalIndex(const std::vector<Domain> &r, double x)
{
    for (size_t i = 0; i < r.size(); ++i)
        if (x >= r[i].first && x <= r[i].second)
            return i;
    return static_cast<size_t>(-1);
}

double paramArea(glm::dvec2 a, glm::dvec2 b, glm::dvec2 c)
{
    const glm::dvec2 ab = b - a;
    const glm::dvec2 ac = c - a;
    return 0.5 * (ab.x * ac.y - ab.y * ac.x);
}

// The sampled chord criterion from the task: at the three edge midpoints and
// the centroid of each parameter triangle, compare the true surface with the
// linear interpolation of the output triangle.
void requireChordCriterion(const std::vector<RationalSurface3d> &patches,
                           const TessellationMesh &mesh, double tolerance)
{
    REQUIRE(mesh.triangles.size() == mesh.triangle_patch.size());
    REQUIRE(mesh.triangles.size() == mesh.triangle_uv.size());
    REQUIRE(!mesh.triangles.empty());
    for (size_t t = 0; t < mesh.triangles.size(); ++t)
    {
        const size_t patch = mesh.triangle_patch[t];
        REQUIRE(patch < patches.size());
        const glm::dvec3 p0 = mesh.positions[mesh.triangles[t][0]];
        const glm::dvec3 p1 = mesh.positions[mesh.triangles[t][1]];
        const glm::dvec3 p2 = mesh.positions[mesh.triangles[t][2]];
        const std::array<glm::dvec2, 3> &uv = mesh.triangle_uv[t];
        const double w[4][3] = {{0.5, 0.5, 0.0}, {0.5, 0.0, 0.5}, {0.0, 0.5, 0.5},
                                {1.0 / 3.0, 1.0 / 3.0, 1.0 / 3.0}};
        for (int m = 0; m < 4; ++m)
        {
            const glm::dvec2 sample =
                w[m][0] * uv[0] + w[m][1] * uv[1] + w[m][2] * uv[2];
            const glm::dvec3 linear = w[m][0] * p0 + w[m][1] * p1 + w[m][2] * p2;
            const glm::dvec3 truth = surfaceAt(patches[patch], sample);
            REQUIRE(glm::length(truth - linear) <=
                    tolerance + 1e-12 * (1.0 + glm::length(truth)));
        }
    }
}

// Checks each triangle lies inside one non-zero knot interval in both
// directions, has positive parameter-space winding, and that the per-cell
// parameter areas match the cells' areas exactly.  Returns the triangle
// count per root cell.
std::vector<std::vector<size_t>> requireCoverage(const RationalSurface3d &srf,
                                                 const TessellationMesh &mesh,
                                                 size_t patch_index)
{
    const std::vector<Domain> ru = roots(srf.knots_u, srf.degree_u, srf.control_points.rows());
    const std::vector<Domain> rv = roots(srf.knots_v, srf.degree_v, srf.control_points.cols());
    std::vector<std::vector<size_t>> counts(ru.size(), std::vector<size_t>(rv.size(), 0));
    std::vector<std::vector<double>> areas(ru.size(), std::vector<double>(rv.size(), 0.0));

    for (size_t t = 0; t < mesh.triangles.size(); ++t)
    {
        if (mesh.triangle_patch[t] != patch_index)
            continue;
        const std::array<glm::dvec2, 3> &uv = mesh.triangle_uv[t];
        const double area = paramArea(uv[0], uv[1], uv[2]);
        REQUIRE(area > 0.0);

        // The three corners must share a common non-zero knot interval.
        size_t common_u = static_cast<size_t>(-1);
        size_t common_v = static_cast<size_t>(-1);
        for (size_t i = 0; i < ru.size(); ++i)
        {
            bool ok = true;
            for (int c = 0; c < 3; ++c)
                ok = ok && uv[c].x >= ru[i].first && uv[c].x <= ru[i].second;
            if (ok)
            {
                common_u = i;
                break;
            }
        }
        for (size_t i = 0; i < rv.size(); ++i)
        {
            bool ok = true;
            for (int c = 0; c < 3; ++c)
                ok = ok && uv[c].y >= rv[i].first && uv[c].y <= rv[i].second;
            if (ok)
            {
                common_v = i;
                break;
            }
        }
        REQUIRE(common_u != static_cast<size_t>(-1));
        REQUIRE(common_v != static_cast<size_t>(-1));

        counts[common_u][common_v] += 1;
        areas[common_u][common_v] += area;
    }

    for (size_t i = 0; i < ru.size(); ++i)
        for (size_t j = 0; j < rv.size(); ++j)
        {
            REQUIRE(counts[i][j] > 0);
            REQUIRE(areas[i][j] ==
                    Approx((ru[i].second - ru[i].first) * (rv[j].second - rv[j].first))
                        .epsilon(1e-12));
        }
    return counts;
}

bool sameSurface(const RationalSurface3d &a, const RationalSurface3d &b){
    if (a.degree_u != b.degree_u || a.degree_v != b.degree_v)
        return false;
    if (a.knots_u != b.knots_u || a.knots_v != b.knots_v)
        return false;
    if (a.control_points.rows() != b.control_points.rows() ||
        a.control_points.cols() != b.control_points.cols())
        return false;
    for (size_t i = 0; i < a.control_points.rows(); ++i)
        for (size_t j = 0; j < a.control_points.cols(); ++j)
            if (a.control_points(i, j) != b.control_points(i, j))
                return false;
    for (size_t i = 0; i < a.weights.rows(); ++i)
        for (size_t j = 0; j < a.weights.cols(); ++j)
            if (a.weights(i, j) != b.weights(i, j))
                return false;
    return true;
}

// Maps a triangle edge (sorted index pair) to the number of triangles of the
// given patch that use it.
std::map<std::pair<size_t, size_t>, size_t> patchEdgeCounts(const TessellationMesh &mesh,
                                                            size_t patch)
{
    std::map<std::pair<size_t, size_t>, size_t> counts;
    for (size_t t = 0; t < mesh.triangles.size(); ++t)
    {
        if (mesh.triangle_patch[t] != patch)
            continue;
        for (int k = 0; k < 3; ++k)
        {
            size_t a = mesh.triangles[t][k];
            size_t b = mesh.triangles[t][(k + 1) % 3];
            if (a > b)
                std::swap(a, b);
            counts[std::make_pair(a, b)] += 1;
        }
    }
    return counts;
}

std::set<size_t> patchVertices(const TessellationMesh &mesh, size_t patch)
{
    std::set<size_t> out;
    for (size_t t = 0; t < mesh.triangles.size(); ++t)
        if (mesh.triangle_patch[t] == patch)
            for (int k = 0; k < 3; ++k)
                out.insert(mesh.triangles[t][k]);
    return out;
}

// Within each patch, every interior triangle edge must be shared by exactly
// two triangles, no triangle corner may lie strictly inside another triangle
// edge (a hanging vertex), and the vertex UVs used for a shared index must
// agree in parameter space (conflicts would mean overlapping or disjoint
// triangles).
void requireConformingWithinPatches(const TessellationMesh &mesh, size_t patch_count)
{
    for (size_t p = 0; p < patch_count; ++p)
    {
        std::map<std::pair<size_t, size_t>, size_t> edge_use;
        std::map<size_t, glm::dvec2> uv_of;
        for (size_t t = 0; t < mesh.triangles.size(); ++t)
        {
            if (mesh.triangle_patch[t] != p)
                continue;
            for (int k = 0; k < 3; ++k)
            {
                const size_t v = mesh.triangles[t][k];
                const glm::dvec2 uv = mesh.triangle_uv[t][k];
                auto it = uv_of.find(v);
                if (it == uv_of.end())
                    uv_of.insert(std::make_pair(v, uv));
                else
                    REQUIRE(glm::length(it->second - uv) < 1e-12);
                size_t a = v;
                size_t b = mesh.triangles[t][(k + 1) % 3];
                if (a > b)
                    std::swap(a, b);
                edge_use[std::make_pair(a, b)] += 1;
            }
        }
        for (const auto &kv : edge_use)
            REQUIRE((kv.second == 1 || kv.second == 2));

        // Detect hanging vertices: a triangle vertex strictly inside an edge.
        for (const auto &edge : edge_use)
        {
            const glm::dvec2 a = uv_of.at(edge.first.first);
            const glm::dvec2 b = uv_of.at(edge.first.second);
            const glm::dvec2 ab = b - a;
            const double len2 = glm::dot(ab, ab);
            if (len2 <= 0.0)
                continue;
            for (const auto &vertex : uv_of)
            {
                if (vertex.first == edge.first.first || vertex.first == edge.first.second)
                    continue;
                const double s = glm::dot(vertex.second - a, ab) / len2;
                if (s <= 1e-12 || s >= 1.0 - 1e-12)
                    continue;
                const glm::dvec2 proj = a + s * ab;
                REQUIRE_FALSE(glm::length(vertex.second - proj) < 1e-12);
            }
        }
    }
}

std::vector<std::pair<glm::dvec2, glm::dvec2>>
seamUvs(const TessellationMesh &mesh, size_t patch_a, size_t patch_b,
        const std::set<size_t> &shared)
{
    std::map<size_t, std::vector<glm::dvec2>> uvs_a, uvs_b;
    for (size_t t = 0; t < mesh.triangles.size(); ++t)
    {
        const size_t patch = mesh.triangle_patch[t];
        if (patch != patch_a && patch != patch_b)
            continue;
        for (int k = 0; k < 3; ++k)
        {
            const size_t v = mesh.triangles[t][k];
            if (shared.count(v) == 0)
                continue;
            if (patch == patch_a)
                uvs_a[v].push_back(mesh.triangle_uv[t][k]);
            else
                uvs_b[v].push_back(mesh.triangle_uv[t][k]);
        }
    }
    std::vector<std::pair<glm::dvec2, glm::dvec2>> pairs;
    for (const auto &kv : uvs_a)
    {
        auto it = uvs_b.find(kv.first);
        if (it == uvs_b.end())
            continue;
        pairs.push_back(std::make_pair(kv.second.front(), it->second.front()));
    }
    return pairs;
}

} // namespace

TEST_CASE("patchmesh flat and curved", "[patchmesh]")
{
    const RationalSurface3d plane = makeUnitPlane();
    const RationalSurface3d bump = makeBump();
    const std::vector<RationalSurface3d> patches{plane, bump};

    TessellationOptions options;
    options.chord_tolerance = 1e-3;
    options.seam_tolerance = 1e-9;
    options.max_depth = 12;

    const TessellationMesh mesh = tessellatePatches(patches, {}, options);
    REQUIRE(!mesh.positions.empty());
    REQUIRE(!mesh.triangles.empty());

    // The sampled chord criterion holds for every output triangle.
    requireChordCriterion(patches, mesh, options.chord_tolerance);

    // No cell has T-junctions or hanging vertices inside a patch.
    requireConformingWithinPatches(mesh, patches.size());

    // Every triangle has a positive parameter winding and every non-zero
    // knot interval is covered exactly once, with no overlap or gap.
    const std::vector<std::vector<size_t>> plane_counts = requireCoverage(plane, mesh, 0);
    const std::vector<std::vector<size_t>> bump_counts = requireCoverage(bump, mesh, 1);

    for (size_t i = 0; i < plane_counts.size(); ++i)
        for (size_t j = 0; j < plane_counts[i].size(); ++j)
            REQUIRE(plane_counts[i][j] == 4);

    // Adaptive density: the flat lower-left unit stays cheap while the curved
    // upper-right unit must be refined beyond eight triangles.
    REQUIRE(bump_counts[0][0] <= 8);
    REQUIRE(bump_counts[1][1] > 8);

    // Without seams, every vertex position is the true surface point of its
    // own UV, and no two patches share an index despite coincident corners.
    for (size_t t = 0; t < mesh.triangles.size(); ++t)
    {
        const size_t patch = mesh.triangle_patch[t];
        for (int k = 0; k < 3; ++k)
        {
            const size_t v = mesh.triangles[t][k];
            const glm::dvec2 uv = mesh.triangle_uv[t][k];
            REQUIRE(glm::length(mesh.positions[v] - surfaceAt(patches[patch], uv)) < 1e-14);
        }
    }
    std::set<size_t> plane_vertices = patchVertices(mesh, 0);
    std::set<size_t> bump_vertices = patchVertices(mesh, 1);
    for (size_t v : plane_vertices)
        REQUIRE(bump_vertices.count(v) == 0);

    // Repeated runs produce identical vertex and triangle ordering.
    const TessellationMesh again = tessellatePatches(patches, {}, options);
    REQUIRE(again.positions.size() == mesh.positions.size());
    REQUIRE(again.triangles.size() == mesh.triangles.size());
    for (size_t i = 0; i < mesh.positions.size(); ++i)
        REQUIRE(again.positions[i] == mesh.positions[i]);
    for (size_t i = 0; i < mesh.triangles.size(); ++i)
    {
        REQUIRE(again.triangles[i] == mesh.triangles[i]);
        REQUIRE(again.triangle_patch[i] == mesh.triangle_patch[i]);
        REQUIRE(again.triangle_uv[i] == mesh.triangle_uv[i]);
    }

    // Empty input without seams yields an empty mesh.
    const TessellationMesh empty = tessellatePatches({}, {}, options);
    REQUIRE(empty.positions.empty());
    REQUIRE(empty.triangles.empty());
    REQUIRE(empty.triangle_patch.empty());
    REQUIRE(empty.triangle_uv.empty());

    // A plain (non-rational) Surface goes through the existing unit-weight
    // conversion constructor and produces the same geometry.
    tinynurbs::Surface3d plain;
    plain.degree_u = 1;
    plain.degree_v = 1;
    plain.knots_u = {0, 0, 1, 1};
    plain.knots_v = {0, 0, 1, 1};
    plain.control_points = {2, 2, {glm::dvec3(0, 0, 0), glm::dvec3(0, 1, 0),
                                   glm::dvec3(1, 0, 0), glm::dvec3(1, 1, 0)}};
    const std::vector<RationalSurface3d> converted{RationalSurface3d(plain)};
    const TessellationMesh plain_mesh = tessellatePatches(converted, {}, options);
    REQUIRE(plain_mesh.triangles.size() == 4);
    for (size_t t = 0; t < plain_mesh.triangles.size(); ++t)
        for (int k = 0; k < 3; ++k)
        {
            const glm::dvec3 uv_point =
                glm::dvec3(plain_mesh.triangle_uv[t][k].x, plain_mesh.triangle_uv[t][k].y, 0.0);
            REQUIRE(glm::length(plain_mesh.positions[plain_mesh.triangles[t][k]] - uv_point) <
                    1e-14);
        }
}

TEST_CASE("patchmesh seams and corners", "[patchmesh]")
{
    // Two quarter-cylinder bands whose UMax/UMin edge is shared.  The second
    // band runs backwards in v and carries extra interior knot spans, so its
    // boundary segmentation differs from the first.
    RationalSurface3d band_a = makeBand(false);
    RationalSurface3d band_b = makeBand(true);
    band_b = tinynurbs::surfaceKnotInsertV(band_b, 11.0, 1);
    band_b = tinynurbs::surfaceKnotInsertV(band_b, 12.0, 1);
    band_b = tinynurbs::surfaceKnotInsertV(band_b, 13.0, 1);
    REQUIRE(band_b.knots_v.size() == band_b.control_points.cols() + band_b.degree_v + 1);

    // Three planar quads meeting at the origin, plus a coincident but
    // undeclared pair far away.
    std::vector<RationalSurface3d> patches;
    patches.push_back(band_a);                                        // 0
    patches.push_back(band_b);                                        // 1
    patches.push_back(makeQuad({0, 0, 0}, {1, 0, 0}, {0, 1, 0}));     // 2
    patches.push_back(makeQuad({0, 0, 0}, {1, 0, 0}, {0, 0, 1}));     // 3
    patches.push_back(makeQuad({0, 0, 0}, {0, 1, 0}, {0, 0, 1}));     // 4
    patches.push_back(makeQuad({10, 0, 0}, {1, 0, 0}, {0, 1, 0}));    // 5
    patches.push_back(makeQuad({10, 0, 0}, {1, 0, 0}, {0, 1, 0}));    // 6

    PatchSeam cylinder;
    cylinder.patch_a = 0;
    cylinder.patch_b = 1;
    cylinder.edge_a = PatchEdge::UMax;
    cylinder.edge_b = PatchEdge::UMin;
    cylinder.reversed = true;

    PatchSeam corner_ab;
    corner_ab.patch_a = 2;
    corner_ab.patch_b = 3;
    corner_ab.edge_a = PatchEdge::VMin;
    corner_ab.edge_b = PatchEdge::VMin;

    PatchSeam corner_bc;
    corner_bc.patch_a = 3;
    corner_bc.patch_b = 4;
    corner_bc.edge_a = PatchEdge::UMin;
    corner_bc.edge_b = PatchEdge::UMin;

    PatchSeam corner_ac;
    corner_ac.patch_a = 2;
    corner_ac.patch_b = 4;
    corner_ac.edge_a = PatchEdge::UMin;
    corner_ac.edge_b = PatchEdge::VMin;

    const std::vector<PatchSeam> seams{cylinder, corner_ab, corner_bc, corner_ac};

    TessellationOptions options;
    options.chord_tolerance = 1e-2;
    options.seam_tolerance = 1e-9;
    options.max_depth = 12;

    const TessellationMesh mesh = tessellatePatches(patches, seams, options);
    REQUIRE(!mesh.triangles.empty());

    // ---- Reversed cylinder seam -------------------------------------------
    std::map<size_t, std::set<size_t>> users;
    for (size_t t = 0; t < mesh.triangles.size(); ++t)
        for (int k = 0; k < 3; ++k)
            users[mesh.triangles[t][k]].insert(mesh.triangle_patch[t]);
    std::set<size_t> cylinder_shared;
    for (const auto &kv : users)
        if (kv.second.count(0) > 0 && kv.second.count(1) > 0)
            cylinder_shared.insert(kv.first);

    const double ua = band_a.knots_u[band_a.knots_u.size() - 1];
    const double ub = band_b.knots_u[band_b.degree_u];
    const double va0 = band_a.knots_v[band_a.degree_v];
    const double va1 = band_a.knots_v[band_a.control_points.cols()];
    const double vb0 = band_b.knots_v[band_b.degree_v];
    const double vb1 = band_b.knots_v[band_b.control_points.cols()];

    // Every shared vertex must really lie on the declared edge of both
    // patches.
    for (size_t v : cylinder_shared)
    {
        bool on_a = false, on_b = false;
        for (size_t t = 0; t < mesh.triangles.size(); ++t)
            for (int k = 0; k < 3; ++k)
                if (mesh.triangles[t][k] == v)
                {
                    const glm::dvec2 uv = mesh.triangle_uv[t][k];
                    if (mesh.triangle_patch[t] == 0 && std::abs(uv.x - ua) < 1e-12)
                        on_a = true;
                    if (mesh.triangle_patch[t] == 1 && std::abs(uv.x - ub) < 1e-12)
                        on_b = true;
                }
        REQUIRE(on_a);
        REQUIRE(on_b);
    }

    // Both sides use the same parameter sites, in the reversed correspondence
    // normalized over each own valid interval.  The union includes the coarse
    // side's sites as well as every site created on the other side.
    const auto pairs = seamUvs(mesh, 0, 1, cylinder_shared);
    REQUIRE(pairs.size() == cylinder_shared.size());
    std::set<double> ta, tb;
    for (const auto &p : pairs)
    {
        const double t_a = (p.first.y - va0) / (va1 - va0);
        const double t_b = (p.second.y - vb0) / (vb1 - vb0);
        ta.insert(t_a);
        tb.insert(t_b);
        REQUIRE(std::abs(t_b - (1.0 - t_a)) < 1e-12);
    }
    REQUIRE(ta.size() >= 9);
    REQUIRE(tb.size() >= 9);
    // The second band's interior knots survive in the shared segmentation.
    for (double t : {0.25, 0.5, 0.75})
        REQUIRE(ta.count(t) == 1);

    // Each seam segment borders exactly one triangle in each of the two
    // patches (the same index pair on both sides).
    const auto edge_a = patchEdgeCounts(mesh, 0);
    const auto edge_b = patchEdgeCounts(mesh, 1);
    std::set<std::pair<size_t, size_t>> seam_segments;
    for (const auto &kv : edge_a)
        if (cylinder_shared.count(kv.first.first) && cylinder_shared.count(kv.first.second))
            seam_segments.insert(kv.first);
    REQUIRE(!seam_segments.empty());
    for (const auto &seg : seam_segments)
    {
        auto ia = edge_a.find(seg);
        auto ib = edge_b.find(seg);
        REQUIRE(ia != edge_a.end());
        REQUIRE(ib != edge_b.end());
        REQUIRE(ia->second == 1);
        REQUIRE(ib->second == 1);
    }
    // No segment of the shared edge is missing from either side.
    for (const auto &kv : edge_b)
        if (cylinder_shared.count(kv.first.first) && cylinder_shared.count(kv.first.second))
            REQUIRE(seam_segments.count(kv.first) == 1);

    // ---- Interleaved seam sites are all carried over ----------------------
    // Patch A is refined along the seam in small steps; patch B uses a coarser
    // step that does not align with every A site.  The union must include
    // every A-only site as a real vertex on B, not just the aligned ones.
    RationalSurface3d fine_a;
    fine_a.degree_u = 1;
    fine_a.degree_v = 1;
    fine_a.knots_u = {0, 0, 1, 1};
    fine_a.knots_v = {0, 0, .05, .1, .15, .2, .25, .3, .35, .4, .45, .5, .55,
                      .6, .65, .7, .75, .8, .85, .9, .95, 1, 1};
    fine_a.control_points.resize(2, 21);
    fine_a.weights = array2<double>(2, 21, 1.0);
    for (size_t i = 0; i < 2; ++i)
        for (size_t j = 0; j < 21; ++j)
            fine_a.control_points(i, j) = glm::dvec3(double(i), double(j) * .05, 0.0);

    RationalSurface3d coarse_b;
    coarse_b.degree_u = 1;
    coarse_b.degree_v = 1;
    coarse_b.knots_u = {0, 0, .1, .2, .3, .4, .5, .6, .7, .8, .9, 1, 1};
    coarse_b.knots_v = {0, 0, 1, 1};
    coarse_b.control_points.resize(11, 2);
    coarse_b.weights = array2<double>(11, 2, 1.0);
    for (size_t i = 0; i < 11; ++i)
        for (size_t j = 0; j < 2; ++j)
            coarse_b.control_points(i, j) = glm::dvec3(1.0 + double(j), double(i) * .1, 0.0);

    PatchSeam interleaved;
    interleaved.patch_a = 0;
    interleaved.patch_b = 1;
    interleaved.edge_a = PatchEdge::UMax;
    interleaved.edge_b = PatchEdge::VMin;
    interleaved.reversed = false;

    const TessellationMesh interleaved_mesh =
        tessellatePatches({fine_a, coarse_b}, {interleaved}, options);

    std::map<size_t, std::set<size_t>> inter_users;
    for (size_t t = 0; t < interleaved_mesh.triangles.size(); ++t)
        for (int k = 0; k < 3; ++k)
            inter_users[interleaved_mesh.triangles[t][k]].insert(
                interleaved_mesh.triangle_patch[t]);
    size_t inter_shared = 0;
    for (const auto &kv : inter_users)
        if (kv.second.count(0) > 0 && kv.second.count(1) > 0)
            ++inter_shared;
    // A contributes 21 seam sites and B 11; the union is the 21-site ladder.
    REQUIRE(inter_shared == 21);

    // Every A seam site must appear as an actual vertex in B's triangles,
    // since B's cell boundaries are split by the union.
    std::set<double> b_seam_params;
    for (size_t t = 0; t < interleaved_mesh.triangles.size(); ++t)
        if (interleaved_mesh.triangle_patch[t] == 1)
            for (int k = 0; k < 3; ++k)
            {
                const glm::dvec2 uv = interleaved_mesh.triangle_uv[t][k];
                if (std::abs(uv.y - 0.0) < 1e-12)
                    b_seam_params.insert(uv.x);
            }
    REQUIRE(b_seam_params.size() == 21);
    for (size_t i = 0; i < 21; ++i)
    {
        bool found = false;
        for (double t : b_seam_params)
            found = found || std::abs(t - 0.05 * double(i)) < 1e-12;
        REQUIRE(found);
    }

    // ---- Corner shared by three patches -----------------------------------
    std::set<size_t> triple;
    for (const auto &kv : users)
        if (kv.second.count(2) > 0 && kv.second.count(3) > 0 && kv.second.count(4) > 0)
            triple.insert(kv.first);
    REQUIRE(triple.size() == 1);
    const size_t corner = *triple.begin();
    REQUIRE(glm::length(mesh.positions[corner] - glm::dvec3(0, 0, 0)) < 1e-12);

    // Each quad is a single cell emitted as a four-triangle fan: four
    // corners plus the cell centre.
    for (size_t patch = 2; patch <= 4; ++patch)
    {
        std::set<size_t> verts = patchVertices(mesh, patch);
        REQUIRE(verts.size() == 5);
        size_t tris = 0;
        for (size_t t = 0; t < mesh.triangles.size(); ++t)
            if (mesh.triangle_patch[t] == patch)
                ++tris;
        REQUIRE(tris == 4);
    }

    // ---- Coincident but unconnected patches must not be welded ------------
    std::set<size_t> p5 = patchVertices(mesh, 5);
    std::set<size_t> p6 = patchVertices(mesh, 6);
    REQUIRE(p5.size() == 5);
    REQUIRE(p6.size() == 5);
    for (size_t v : p5)
        REQUIRE(p6.count(v) == 0);
    // Positions nevertheless coincide because the input geometry is identical.
    for (size_t v : p5)
    {
        bool found = false;
        for (size_t w : p6)
            if (glm::length(mesh.positions[v] - mesh.positions[w]) < 1e-12)
                found = true;
        REQUIRE(found);
    }

    // ---- Sampled chord criterion across the whole result ------------------
    requireChordCriterion(patches, mesh, options.chord_tolerance);

    // ---- No T-junctions or hanging vertices inside any patch --------------
    requireConformingWithinPatches(mesh, patches.size());
}

TEST_CASE("patchmesh errors and limits", "[patchmesh]")
{
    TessellationOptions options;
    options.chord_tolerance = 1e-3;
    options.seam_tolerance = 1e-9;
    options.max_depth = 12;

    SECTION("malformed surfaces are rejected with invalid_argument")
    {
        // Knot vector length inconsistent with degree and control points.
        RationalSurface3d short_knots = makeUnitPlane();
        short_knots.degree_u = 3;
        REQUIRE_THROWS_AS(tessellatePatches({short_knots}, {}, options),
                          std::invalid_argument);

        // Non-monotonic knots.
        RationalSurface3d reversed = makeUnitPlane();
        reversed.knots_u = {1, 0, 1, 1};
        REQUIRE_THROWS_AS(tessellatePatches({reversed}, {}, options),
                          std::invalid_argument);

        // Non-finite knot.
        RationalSurface3d nan_knot = makeUnitPlane();
        nan_knot.knots_u[1] = std::nan("");
        REQUIRE_THROWS_AS(tessellatePatches({nan_knot}, {}, options),
                          std::invalid_argument);

        // Interior knot multiplicity above the degree.
        RationalSurface3d multiplicity = makeUnitPlane();
        multiplicity.degree_u = 1;
        multiplicity.knots_u = {0, 0, .5, .5, 1, 1};
        multiplicity.control_points.resize(3, 2);
        multiplicity.weights = array2<double>(3, 2, 1.0);
        for (size_t i = 0; i < 3; ++i)
            for (size_t j = 0; j < 2; ++j)
                multiplicity.control_points(i, j) = glm::dvec3(double(i) * .5, double(j), 0);
        REQUIRE_THROWS_AS(tessellatePatches({multiplicity}, {}, options),
                          std::invalid_argument);

        // Non-clamped knot vector.
        RationalSurface3d unclamped = makeUnitPlane();
        unclamped.knots_u = {-1, 0, 1, 2};
        REQUIRE_THROWS_AS(tessellatePatches({unclamped}, {}, options),
                          std::invalid_argument);

        // Weights that are zero, negative, non-finite or mis-sized.
        for (double w : {0.0, -1.0, std::nan("")})
        {
            RationalSurface3d bad_weight = makeUnitPlane();
            bad_weight.weights(0, 0) = w;
            REQUIRE_THROWS_AS(tessellatePatches({bad_weight}, {}, options),
                              std::invalid_argument);
        }
        RationalSurface3d mis_sized_weights = makeUnitPlane();
        mis_sized_weights.weights = array2<double>(2, 3, 1.0);
        REQUIRE_THROWS_AS(tessellatePatches({mis_sized_weights}, {}, options),
                          std::invalid_argument);

        // Non-finite control point.
        RationalSurface3d bad_cp = makeUnitPlane();
        bad_cp.control_points(0, 0).x = std::nan("");
        REQUIRE_THROWS_AS(tessellatePatches({bad_cp}, {}, options),
                          std::invalid_argument);

        // Degree outside the supported 1..3 range.
        RationalSurface3d bad_degree = makeUnitPlane();
        bad_degree.degree_u = 4;
        REQUIRE_THROWS_AS(tessellatePatches({bad_degree}, {}, options),
                          std::invalid_argument);
    }

    SECTION("seam declarations and options are validated before evaluation")
    {
        // Out-of-range patch index.
        PatchSeam oob;
        oob.patch_a = 0;
        oob.patch_b = 3;
        REQUIRE_THROWS_AS(tessellatePatches({makeUnitPlane(), makeUnitPlane()}, {oob}, options),
                          std::invalid_argument);

        // Edge joined to itself through the same patch.
        PatchSeam self;
        self.patch_a = 0;
        self.patch_b = 0;
        self.edge_a = PatchEdge::UMin;
        self.edge_b = PatchEdge::UMax;
        REQUIRE_THROWS_AS(tessellatePatches({makeUnitPlane()}, {self}, options),
                          std::invalid_argument);

        // The same edge used by two seams, with reversed roles.
        PatchSeam first;
        first.patch_a = 0;
        first.patch_b = 1;
        first.edge_a = PatchEdge::UMax;
        first.edge_b = PatchEdge::UMin;
        PatchSeam duplicate = first;
        duplicate.patch_a = 1;
        duplicate.patch_b = 0;
        duplicate.edge_a = PatchEdge::UMin;
        duplicate.edge_b = PatchEdge::UMax;
        REQUIRE_THROWS_AS(
            tessellatePatches({makeUnitPlane(), makeUnitPlane()}, {first, duplicate}, options),
            std::invalid_argument);

        // Invalid PatchEdge value.
        PatchSeam bad_edge;
        bad_edge.patch_a = 0;
        bad_edge.patch_b = 1;
        bad_edge.edge_a = static_cast<PatchEdge>(7);
        bad_edge.edge_b = PatchEdge::UMin;
        REQUIRE_THROWS_AS(
            tessellatePatches({makeUnitPlane(), makeUnitPlane()}, {bad_edge}, options),
            std::invalid_argument);

        // Invalid options.
        for (double tol : {0.0, -1.0, std::nan(""), std::numeric_limits<double>::infinity()})
        {
            TessellationOptions bad = options;
            bad.chord_tolerance = tol;
            REQUIRE_THROWS_AS(tessellatePatches({makeUnitPlane()}, {}, bad),
                              std::invalid_argument);
        }
        TessellationOptions bad_seam = options;
        bad_seam.seam_tolerance = 0.0;
        REQUIRE_THROWS_AS(tessellatePatches({makeUnitPlane()}, {}, bad_seam),
                          std::invalid_argument);
    }

    SECTION("unmet tolerances fail with runtime_error and leave inputs untouched")
    {
        const RationalSurface3d bump = makeBump();
        const RationalSurface3d bump_copy = makeBump();

        // Depth 0 forbids any subdivision: the curved unit cannot meet the
        // chord tolerance.
        TessellationOptions no_depth = options;
        no_depth.max_depth = 0;
        REQUIRE_THROWS_AS(tessellatePatches({bump}, {}, no_depth), std::runtime_error);
        REQUIRE(sameSurface(bump, bump_copy));

        // A degenerate (zero-length) parameter region.
        RationalSurface3d degenerate = makeUnitPlane();
        degenerate.knots_u = {2, 2, 2, 2};
        REQUIRE_THROWS_AS(tessellatePatches({degenerate}, {}, options), std::runtime_error);

        // Declared seam whose geometry disagrees beyond seam_tolerance.
        RationalSurface3d offset = makeUnitPlane();
        offset.control_points = {2, 2, {glm::dvec3(0, 0, 0), glm::dvec3(0, 1, 0),
                                       glm::dvec3(1, 0, .1), glm::dvec3(1, 1, .1)}};
        PatchSeam seam;
        seam.patch_a = 0;
        seam.patch_b = 1;
        seam.edge_a = PatchEdge::UMax;
        seam.edge_b = PatchEdge::UMin;
        REQUIRE_THROWS_AS(tessellatePatches({makeUnitPlane(), offset}, {seam}, options),
                          std::runtime_error);

        // Extremely small chord tolerance exhausts max_depth.  A small depth
        // keeps the rejected recursion bounded; 0 and 3 are both insufficient
        // for this curvature, so the failure is a depth failure either way.
        TessellationOptions too_tight = options;
        too_tight.chord_tolerance = 1e-300;
        too_tight.max_depth = 3;
        REQUIRE_THROWS_AS(tessellatePatches({makeBand(false)}, {}, too_tight),
                          std::runtime_error);
        // Depth 0 fails immediately on an even more forgiving tolerance.
        TessellationOptions shallow = options;
        shallow.max_depth = 0;
        REQUIRE_THROWS_AS(tessellatePatches({makeBump()}, {}, shallow), std::runtime_error);
    }

    SECTION("successful calls also leave inputs untouched")
    {
        const std::vector<RationalSurface3d> patches{makeUnitPlane(), makeBump()};
        const std::vector<RationalSurface3d> copies{makeUnitPlane(), makeBump()};
        tessellatePatches(patches, {}, options);
        for (size_t i = 0; i < patches.size(); ++i)
            REQUIRE(sameSurface(patches[i], copies[i]));
    }
}
