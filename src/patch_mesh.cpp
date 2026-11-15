/**
 * Indexed triangle mesh generation for PatchEditor snapshots.
 *
 * Use of this source code is governed by a BSD-style license that can be found in
 * the LICENSE file.
 */

#include "tinynurbs/core/patch_mesh.h"

#include "tinynurbs/core/evaluate.h"
#include "tinynurbs/core/patch_editor.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <map>
#include <numeric>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace tinynurbs
{

namespace
{

using dvec3 = glm::dvec3;

constexpr int kGridPerCell = 4;
constexpr double kStationTolerance = 1e-9;

bool finitePoint(const dvec3 &p)
{
    return std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z);
}

struct UnionFind
{
    std::vector<int> parent;
    explicit UnionFind(std::size_t n) : parent(n)
    {
        std::iota(parent.begin(), parent.end(), 0);
    }
    int find(int x)
    {
        while (parent[static_cast<std::size_t>(x)] != x)
        {
            parent[static_cast<std::size_t>(x)] =
                parent[static_cast<std::size_t>(parent[static_cast<std::size_t>(x)])];
            x = parent[static_cast<std::size_t>(x)];
        }
        return x;
    }
    void unite(int a, int b)
    {
        a = find(a);
        b = find(b);
        if (a != b)
        {
            parent[static_cast<std::size_t>(b)] = a;
        }
    }
};

std::pair<double, double> domainOf(const std::vector<double> &knots, unsigned int degree)
{
    const std::size_t num_cp = knots.size() - degree - 1;
    return std::make_pair(knots[degree], knots[num_cp]);
}

// Distinct interior knots (plus domain endpoints) of a clamped knot vector.
std::vector<double> cellBreaks(const std::vector<double> &knots, unsigned int degree)
{
    const auto dom = domainOf(knots, degree);
    std::vector<double> vals;
    for (double k : knots)
    {
        if (k > dom.first && k < dom.second)
        {
            vals.push_back(k);
        }
    }
    std::sort(vals.begin(), vals.end());
    std::vector<double> out;
    out.push_back(dom.first);
    for (double v : vals)
    {
        const double tol = 1e-12 * std::max(1.0, std::max(std::abs(v), std::abs(dom.first)));
        if (v > out.back() + tol)
        {
            out.push_back(v);
        }
    }
    out.push_back(dom.second);
    return out;
}

void insertNormalized(std::vector<double> &set, double t)
{
    for (double v : set)
    {
        if (std::abs(v - t) <= kStationTolerance)
        {
            return;
        }
    }
    set.push_back(t);
}

std::vector<double> baseNormStations(const std::vector<double> &knots, unsigned int degree)
{
    const auto dom = domainOf(knots, degree);
    const double span = dom.second - dom.first;
    const std::vector<double> breaks = cellBreaks(knots, degree);
    std::vector<double> out;
    for (std::size_t c = 0; c + 1 < breaks.size(); ++c)
    {
        for (int r = 0; r < kGridPerCell; ++r)
        {
            const double val = breaks[c] + (breaks[c + 1] - breaks[c]) *
                                               (static_cast<double>(r) / kGridPerCell);
            insertNormalized(out, (val - dom.first) / span);
        }
    }
    insertNormalized(out, 1.0);
    std::sort(out.begin(), out.end());
    return out;
}

double nearestIndex(const std::vector<double> &sorted, double value)
{
    std::size_t best = 0;
    double bestDist = std::abs(sorted[0] - value);
    for (std::size_t i = 1; i < sorted.size(); ++i)
    {
        const double d = std::abs(sorted[i] - value);
        if (d < bestDist)
        {
            bestDist = d;
            best = i;
        }
    }
    return static_cast<double>(best);
}

bool isAlongU(SurfaceEdge edge)
{
    return edge == SurfaceEdge::VMin || edge == SurfaceEdge::VMax;
}

bool isMinEdge(SurfaceEdge edge) { return edge == SurfaceEdge::UMin || edge == SurfaceEdge::VMin; }

} // namespace

PatchMesh buildPatchMesh(const PatchEditorSnapshot &snapshot)
{
    PatchMesh mesh;
    const std::size_t patch_count = snapshot.patches.size();
    if (patch_count == 0)
    {
        return mesh;
    }

    std::vector<std::vector<double>> knots_u(patch_count);
    std::vector<std::vector<double>> knots_v(patch_count);
    for (std::size_t p = 0; p < patch_count; ++p)
    {
        if (snapshot.original_knots_u.size() == patch_count &&
            snapshot.original_knots_v.size() == patch_count)
        {
            knots_u[p] = snapshot.original_knots_u[p];
            knots_v[p] = snapshot.original_knots_v[p];
        }
        else
        {
            knots_u[p] = snapshot.patches[p].knots_u;
            knots_v[p] = snapshot.patches[p].knots_v;
        }
        if (knots_u[p].empty() || knots_v[p].empty())
        {
            knots_u[p] = snapshot.patches[p].knots_u;
            knots_v[p] = snapshot.patches[p].knots_v;
        }
    }

    // Normalized along-direction station sets, refined until every declared
    // seam uses the same set on both sides (in A's orientation).
    std::vector<std::vector<double>> norm_u(patch_count), norm_v(patch_count);
    for (std::size_t p = 0; p < patch_count; ++p)
    {
        norm_u[p] = baseNormStations(knots_u[p], snapshot.patches[p].degree_u);
        norm_v[p] = baseNormStations(knots_v[p], snapshot.patches[p].degree_v);
    }

    auto alongSet = [&](int patch, SurfaceEdge edge) -> std::vector<double> & {
        return isAlongU(edge) ? norm_u[static_cast<std::size_t>(patch)]
                             : norm_v[static_cast<std::size_t>(patch)];
    };

    for (int iteration = 0; iteration < 128; ++iteration)
    {
        bool changed = false;
        for (const PatchSeam &seam : snapshot.seams)
        {
            std::vector<double> &sa = alongSet(seam.patch_a, seam.edge_a);
            std::vector<double> &sb = alongSet(seam.patch_b, seam.edge_b);
            std::vector<double> merged = sa;
            for (double t : sb)
            {
                insertNormalized(merged, seam.reversed ? 1.0 - t : t);
            }
            std::sort(merged.begin(), merged.end());
            auto assign = [&](std::vector<double> &target, bool reverse) {
                std::vector<double> want = merged;
                if (reverse)
                {
                    for (double &t : want)
                    {
                        t = 1.0 - t;
                    }
                }
                std::sort(want.begin(), want.end());
                if (want.size() != target.size())
                {
                    target = want;
                    changed = true;
                }
                else
                {
                    for (std::size_t i = 0; i < want.size(); ++i)
                    {
                        if (std::abs(want[i] - target[i]) > kStationTolerance)
                        {
                            target = want;
                            changed = true;
                            break;
                        }
                    }
                }
            };
            assign(sa, false);
            assign(sb, seam.reversed);
        }
        if (!changed)
        {
            break;
        }
    }

    // Final station values in each patch's original parameter domain.
    std::vector<std::vector<double>> us(patch_count), vs(patch_count);
    std::vector<std::vector<std::vector<int>>> grid_id(
        patch_count); // [patch][i][j] -> global vertex id
    std::size_t vertex_count = 0;
    for (std::size_t p = 0; p < patch_count; ++p)
    {
        const auto dom_u = domainOf(knots_u[p], snapshot.patches[p].degree_u);
        const auto dom_v = domainOf(knots_v[p], snapshot.patches[p].degree_v);
        us[p].resize(norm_u[p].size());
        for (std::size_t i = 0; i < norm_u[p].size(); ++i)
        {
            us[p][i] = dom_u.first + norm_u[p][i] * (dom_u.second - dom_u.first);
        }
        vs[p].resize(norm_v[p].size());
        for (std::size_t j = 0; j < norm_v[p].size(); ++j)
        {
            vs[p][j] = dom_v.first + norm_v[p][j] * (dom_v.second - dom_v.first);
        }
        grid_id[p].assign(us[p].size(), std::vector<int>(vs[p].size(), -1));
        for (std::size_t i = 0; i < us[p].size(); ++i)
        {
            for (std::size_t j = 0; j < vs[p].size(); ++j)
            {
                grid_id[p][i][j] = static_cast<int>(vertex_count++);
            }
        }
    }

    UnionFind weld(vertex_count);
    for (const PatchSeam &seam : snapshot.seams)
    {
        const std::size_t pa = static_cast<std::size_t>(seam.patch_a);
        const std::size_t pb = static_cast<std::size_t>(seam.patch_b);
        const bool along_u_a = isAlongU(seam.edge_a);
        const bool along_u_b = isAlongU(seam.edge_b);

        const std::vector<double> &na = along_u_a ? norm_u[pa] : norm_v[pa];
        const std::vector<double> &nb = along_u_b ? norm_u[pb] : norm_v[pb];

        const int a_line = isMinEdge(seam.edge_a)
                               ? 0
                               : static_cast<int>(along_u_a ? vs[pa].size() - 1
                                                            : us[pa].size() - 1);
        const int b_line = isMinEdge(seam.edge_b)
                               ? 0
                               : static_cast<int>(along_u_b ? vs[pb].size() - 1
                                                            : us[pb].size() - 1);

        for (double t : na)
        {
            const double tb = seam.reversed ? (1.0 - t) : t;
            const int ia = static_cast<int>(nearestIndex(na, t));
            const int ib = static_cast<int>(nearestIndex(nb, tb));
            if (std::abs(na[static_cast<std::size_t>(ia)] - t) > kStationTolerance ||
                std::abs(nb[static_cast<std::size_t>(ib)] - tb) > kStationTolerance)
            {
                continue;
            }
            const int va = along_u_a ? grid_id[pa][static_cast<std::size_t>(ia)]
                                                 [static_cast<std::size_t>(a_line)]
                                     : grid_id[pa][static_cast<std::size_t>(a_line)]
                                              [static_cast<std::size_t>(ia)];
            const int vb = along_u_b ? grid_id[pb][static_cast<std::size_t>(ib)]
                                                 [static_cast<std::size_t>(b_line)]
                                     : grid_id[pb][static_cast<std::size_t>(b_line)]
                                              [static_cast<std::size_t>(ib)];
            weld.unite(va, vb);
        }
    }

    // Positions: average coincident welded points so shared vertices are exact.
    std::vector<dvec3> accumulated(vertex_count, dvec3(0.0));
    std::vector<int> counts(vertex_count, 0);
    for (std::size_t p = 0; p < patch_count; ++p)
    {
        for (std::size_t i = 0; i < us[p].size(); ++i)
        {
            for (std::size_t j = 0; j < vs[p].size(); ++j)
            {
                const int id = grid_id[p][i][j];
                const dvec3 pt = surfacePoint(snapshot.patches[p], us[p][i], vs[p][j]);
                accumulated[static_cast<std::size_t>(id)] += pt;
                counts[static_cast<std::size_t>(id)] += 1;
            }
        }
    }
    std::vector<int> root_to_position(vertex_count, -1);
    std::vector<dvec3> root_sum(vertex_count, dvec3(0.0));
    std::vector<int> root_count(vertex_count, 0);
    for (std::size_t id = 0; id < vertex_count; ++id)
    {
        const int r = weld.find(static_cast<int>(id));
        root_sum[static_cast<std::size_t>(r)] += accumulated[id];
        root_count[static_cast<std::size_t>(r)] += 1;
    }
    for (std::size_t id = 0; id < vertex_count; ++id)
    {
        const int r = weld.find(static_cast<int>(id));
        if (root_to_position[static_cast<std::size_t>(r)] < 0)
        {
            dvec3 pos = root_sum[static_cast<std::size_t>(r)] /
                        static_cast<double>(root_count[static_cast<std::size_t>(r)]);
            root_to_position[static_cast<std::size_t>(r)] = static_cast<int>(mesh.positions.size());
            mesh.positions.push_back(pos);
        }
    }

    auto makeCorner = [&](std::size_t p, std::size_t i, std::size_t j) {
        PatchMeshCorner corner;
        corner.position = root_to_position[static_cast<std::size_t>(
            weld.find(grid_id[p][i][j]))];
        corner.patch = static_cast<int>(p);
        corner.u = us[p][i];
        corner.v = vs[p][j];
        const dvec3 n = surfaceNormal(snapshot.patches[p], corner.u, corner.v);
        corner.normal = finitePoint(n) ? n : dvec3(0.0);
        return corner;
    };

    for (std::size_t p = 0; p < patch_count; ++p)
    {
        for (std::size_t i = 0; i + 1 < us[p].size(); ++i)
        {
            for (std::size_t j = 0; j + 1 < vs[p].size(); ++j)
            {
                const PatchMeshCorner c00 = makeCorner(p, i, j);
                const PatchMeshCorner c10 = makeCorner(p, i + 1, j);
                const PatchMeshCorner c11 = makeCorner(p, i + 1, j + 1);
                const PatchMeshCorner c01 = makeCorner(p, i, j + 1);

                PatchMeshTriangle t0;
                t0.corners[0] = c00;
                t0.corners[1] = c10;
                t0.corners[2] = c11;
                mesh.triangles.push_back(t0);

                PatchMeshTriangle t1;
                t1.corners[0] = c00;
                t1.corners[1] = c11;
                t1.corners[2] = c01;
                mesh.triangles.push_back(t1);
            }
        }
    }
    return mesh;
}

bool writePatchMeshOBJ(const PatchMesh &mesh, const std::string &path)
{
    std::ofstream out(path);
    if (!out)
    {
        return false;
    }
    out.setf(std::ios::fixed, std::ios::floatfield);
    out.precision(12);
    for (const dvec3 &p : mesh.positions)
    {
        out << "v " << p.x << ' ' << p.y << ' ' << p.z << '\n';
    }
    // One analytic normal per triangle corner.  Distinct corners of the same
    // welded position keep their own patch normal.
    for (const PatchMeshTriangle &t : mesh.triangles)
    {
        for (int c = 0; c < 3; ++c)
        {
            const dvec3 &n = t.corners[c].normal;
            out << "vn " << n.x << ' ' << n.y << ' ' << n.z << '\n';
        }
    }
    std::size_t normal_index = 1;
    for (const PatchMeshTriangle &t : mesh.triangles)
    {
        out << "f";
        for (int c = 0; c < 3; ++c)
        {
            out << ' ' << (t.corners[c].position + 1) << "//" << normal_index++;
        }
        out << '\n';
    }
    return static_cast<bool>(out);
}

bool writePatchMeshOBJ(const PatchEditorSnapshot &snapshot, const std::string &path)
{
    return writePatchMeshOBJ(buildPatchMesh(snapshot), path);
}

} // namespace tinynurbs
