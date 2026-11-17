// Regression tests for HGrid region editing and sparse surface extraction.
//
// The file is compiled together with task_support/driver.c, which supplies
// main() and walks the task_cases table below. Cases are grouped as
//   edits       - HGrid::fill_box semantics and copy independence
//   surface     - sparse volume_polygonize vs. the dense RGrid reference
//   sparse_work - work counters for large, mostly uniform scenes
//
// Every assertion goes through TASK_CHECK, which counts the check and makes
// the case fail (return 1) when the condition is false.

#include "task_support/check.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include <GEL/CGLA/Vec.h>
#include <GEL/Geometry/HGrid.h>
#include <GEL/Geometry/RGrid.h>
#include <GEL/Geometry/XForm.h>
#include <GEL/HMesh/Manifold.h>
#include <GEL/HMesh/polygonize.h>

using namespace CGLA;
using namespace Geometry;
using namespace HMesh;

namespace {

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

float field_value(int x, int y, int z)
{
    // An asymmetric scalar field that crosses several top level bricks. The
    // two NaN bands are the missing samples which must become holes.
    if (x == 4 && y == 5 && z == 3)
        return std::nanf("");
    if (x == 9 && y == 9)
        return std::nanf("");
    if (x == 12 && z == 2 && (y % 3) == 0)
        return std::nanf("");
    return float(x * 7 + y * 3 - z * 5 - 60);
}

std::vector<float> flatten(const RGrid<float>& g, const Vec3i& dims)
{
    std::vector<float> out(static_cast<size_t>(dims[0]) * dims[1] * dims[2]);
    for (int z = 0; z < dims[2]; ++z)
        for (int y = 0; y < dims[1]; ++y)
            for (int x = 0; x < dims[0]; ++x)
                out[(static_cast<size_t>(z) * dims[1] + y) * dims[0] + x] =
                    g[Vec3i(x, y, z)];
    return out;
}

bool grid_matches(const HGrid<float>& g, const Vec3i& dims,
                  const std::vector<float>& expected)
{
    for (int z = 0; z < dims[2]; ++z)
        for (int y = 0; y < dims[1]; ++y)
            for (int x = 0; x < dims[0]; ++x)
            {
                const float a = g[Vec3i(x, y, z)];
                const float e =
                    expected[(static_cast<size_t>(z) * dims[1] + y) * dims[0] + x];
                // NaN is part of the data in some tests; compare bit patterns
                // when either side is NaN.
                if (std::isnan(a) || std::isnan(e)) {
                    if (!(std::isnan(a) && std::isnan(e)))
                        return false;
                }
                else if (a != e)
                    return false;
            }
    return true;
}

// ---------------------------------------------------------------------------
// Oriented surface comparison (tolerance based, order independent)
// ---------------------------------------------------------------------------

bool close_pos(const Vec3d& a, const Vec3d& b, double tol)
{
    return std::fabs(a[0] - b[0]) <= tol &&
           std::fabs(a[1] - b[1]) <= tol &&
           std::fabs(a[2] - b[2]) <= tol;
}

bool lex_less(const Vec3d& a, const Vec3d& b)
{
    for (int i = 0; i < 3; ++i) {
        if (a[i] < b[i]) return true;
        if (a[i] > b[i]) return false;
    }
    return false;
}

// Rotate a face so the lexicographically smallest vertex comes first. The
// orientation (cyclic order) is preserved.
std::vector<Vec3d> canonical_face(std::vector<Vec3d> vs)
{
    if (vs.empty())
        return vs;
    size_t best = 0;
    for (size_t i = 1; i < vs.size(); ++i)
        if (lex_less(vs[i], vs[best]))
            best = i;
    std::rotate(vs.begin(), vs.begin() + best, vs.end());
    return vs;
}

std::vector<std::vector<Vec3d>> manifold_faces(const Manifold& m)
{
    std::vector<std::vector<Vec3d>> faces;
    faces.reserve(m.no_faces());
    for (FaceID f : m.faces()) {
        std::vector<Vec3d> vs;
        for (VertexID v : m.incident_vertices(f))
            vs.push_back(m.pos(v));
        faces.push_back(canonical_face(std::move(vs)));
    }
    return faces;
}

std::string face_key(const std::vector<Vec3d>& face)
{
    char buf[64];
    std::string key;
    for (const Vec3d& p : face) {
        for (int i = 0; i < 3; ++i) {
            std::snprintf(buf, sizeof(buf), "%.6f,", p[i]);
            key += buf;
        }
        key += ';';
    }
    return key;
}

// Compare two meshes as oriented polygonal surfaces. Vertex numbering and
// face traversal order are ignored; positions are compared with tol.
bool same_oriented_surface(const Manifold& a, const Manifold& b, double tol)
{
    if (a.no_vertices() != b.no_vertices())
        return false;
    if (a.no_faces() != b.no_faces())
        return false;

    auto fa = manifold_faces(a);
    auto fb = manifold_faces(b);
    if (fa.size() != fb.size())
        return false;

    std::map<std::string, std::vector<size_t>> buckets;
    for (size_t i = 0; i < fb.size(); ++i)
        buckets[face_key(fb[i])].push_back(i);
    std::vector<char> used(fb.size(), 0);

    for (const auto& face : fa) {
        auto it = buckets.find(face_key(face));
        if (it == buckets.end())
            return false;

        bool matched = false;
        for (size_t idx : it->second) {
            if (used[idx])
                continue;
            const auto& other = fb[idx];
            if (other.size() != face.size())
                continue;
            const size_t n = face.size();
            for (size_t rot = 0; rot < n && !matched; ++rot) {
                bool ok = true;
                for (size_t i = 0; i < n; ++i)
                    if (!close_pos(face[i], other[(i + rot) % n], tol)) {
                        ok = false;
                        break;
                    }
                if (ok) {
                    used[idx] = 1;
                    matched = true;
                }
            }
            if (matched)
                break;
        }
        if (!matched)
            return false;
    }
    return true;
}

bool mesh_is_closed(const Manifold& m)
{
    for (HalfEdgeID h : m.halfedges())
        if (m.walker(h).face() == InvalidFaceID)
            return false;
    return true;
}

Vec3d mesh_min(const Manifold& m)
{
    Vec3d lo(1e300);
    for (VertexID v : m.vertices())
        for (int i = 0; i < 3; ++i)
            lo[i] = std::min(lo[i], m.pos(v)[i]);
    return lo;
}

Vec3d mesh_max(const Manifold& m)
{
    Vec3d hi(-1e300);
    for (VertexID v : m.vertices())
        for (int i = 0; i < 3; ++i)
            hi[i] = std::max(hi[i], m.pos(v)[i]);
    return hi;
}

// Build an RGrid from a callable value(x,y,z).
template <class F>
RGrid<float> make_rgrid(const Vec3i& dims, F value)
{
    RGrid<float> g(dims, 0.0f);
    for (int z = 0; z < dims[2]; ++z)
        for (int y = 0; y < dims[1]; ++y)
            for (int x = 0; x < dims[0]; ++x)
                g.store(Vec3i(x, y, z), value(x, y, z));
    return g;
}

template <class F>
HGrid<float> make_hgrid(const Vec3i& dims, F value)
{
    HGrid<float> g(dims, 0.0f);
    for (int z = 0; z < dims[2]; ++z)
        for (int y = 0; y < dims[1]; ++y)
            for (int x = 0; x < dims[0]; ++x)
                g.store(Vec3i(x, y, z), value(x, y, z));
    return g;
}

// ---------------------------------------------------------------------------
// edits group
// ---------------------------------------------------------------------------

int case_fill_box_arrays()
{
    const Vec3i dims(13, 10, 9);
    const float initial = -3.0f;
    HGrid<float> g(dims, initial);

    std::vector<float> expected(
        static_cast<size_t>(dims[0]) * dims[1] * dims[2], initial);

    auto fill_expected = [&](Vec3i lo, Vec3i hi, float value) {
        lo = v_max(lo, Vec3i(0));
        hi = v_min(hi, dims);
        if (lo[0] >= hi[0] || lo[1] >= hi[1] || lo[2] >= hi[2])
            return;
        for (int z = lo[2]; z < hi[2]; ++z)
            for (int y = lo[1]; y < hi[1]; ++y)
                for (int x = lo[0]; x < hi[0]; ++x)
                    expected[(static_cast<size_t>(z) * dims[1] + y) * dims[0] + x] =
                        value;
    };

    struct Op { Vec3i lo, hi; float value; };
    const Op ops[] = {
        // Clipped on the negative side.
        {Vec3i(-2, -1, 1), Vec3i(5, 4, 7), 2.0f},
        // Overlaps the previous box and is clipped on the positive side.
        {Vec3i(6, 3, 2), Vec3i(20, 20, 20), 4.0f},
        // A single voxel in the last (partial) brick.
        {Vec3i(12, 9, 8), Vec3i(13, 10, 9), 5.0f},
        // Empty intersections leave the grid unchanged.
        {Vec3i(-5, -5, -5), Vec3i(0, 0, 0), 9.0f},
        {Vec3i(20, 20, 20), Vec3i(30, 30, 30), 9.0f},
        // An empty box along one axis only.
        {Vec3i(2, 2, 2), Vec3i(2, 8, 8), 9.0f},
    };

    for (const Op& op : ops) {
        g.fill_box(op.lo, op.hi, op.value);
        fill_expected(op.lo, op.hi, op.value);
        TASK_CHECK(grid_matches(g, dims, expected));
    }

    // Values outside the boxes keep their original value.
    TASK_CHECK(g[Vec3i(0, 0, 0)] == initial);
    TASK_CHECK(g[Vec3i(6, 0, 0)] == initial);
    TASK_CHECK(g[Vec3i(0, 5, 0)] == initial);
    TASK_CHECK(g[Vec3i(12, 0, 8)] == initial);
    return 0;
}

int case_fill_box_brick_merge()
{
    const Vec3i dims(16, 16, 16);
    HGrid<float> g(dims, 0.0f);

    // Top level brick (0,0,0) is covered completely: it must be merged, not
    // split and written voxel by voxel.
    g.fill_box(Vec3i(0, 0, 0), Vec3i(8, 8, 8), 7.0f);
    TASK_CHECK(g.get_cell(Vec3i(0, 0, 0)).is_coalesced());
    TASK_CHECK(g[Vec3i(0, 0, 0)] == 7.0f);
    TASK_CHECK(g[Vec3i(7, 7, 7)] == 7.0f);
    TASK_CHECK(g[Vec3i(8, 0, 0)] == 0.0f);

    // A partial fill inside a merged brick does not change it.
    g.fill_box(Vec3i(1, 1, 1), Vec3i(2, 2, 2), 7.0f);
    TASK_CHECK(g.get_cell(Vec3i(0, 0, 0)).is_coalesced());

    // A partial fill that changes something splits the brick.
    g.fill_box(Vec3i(3, 3, 3), Vec3i(4, 4, 4), 2.0f);
    TASK_CHECK(!g.get_cell(Vec3i(0, 0, 0)).is_coalesced());
    TASK_CHECK(g[Vec3i(3, 3, 3)] == 2.0f);
    TASK_CHECK(g[Vec3i(2, 2, 2)] == 7.0f);
    TASK_CHECK(g[Vec3i(4, 3, 3)] == 7.0f);

    // Refilling the whole brick with one value must merge it again.
    g.fill_box(Vec3i(0, 0, 0), Vec3i(8, 8, 8), 7.0f);
    TASK_CHECK(g.get_cell(Vec3i(0, 0, 0)).is_coalesced());
    TASK_CHECK(g[Vec3i(3, 3, 3)] == 7.0f);

    // A box that only covers part of a brick does not merge it unless the
    // remaining voxels already agree. Brick (1,1,1) still holds 0 outside
    // the box, so filling a single voxel with 7 must not merge it ...
    g.fill_box(Vec3i(8, 8, 8), Vec3i(9, 9, 9), 7.0f);
    TASK_CHECK(!g.get_cell(Vec3i(1, 1, 1)).is_coalesced());
    TASK_CHECK(g[Vec3i(8, 8, 8)] == 7.0f);
    TASK_CHECK(g[Vec3i(9, 8, 8)] == 0.0f);
    // ... but once the whole brick is uniform again it must merge.
    g.fill_box(Vec3i(8, 8, 8), Vec3i(16, 16, 16), 7.0f);
    TASK_CHECK(g.get_cell(Vec3i(1, 1, 1)).is_coalesced());
    TASK_CHECK(g[Vec3i(8, 8, 8)] == 7.0f);

    // Changing one voxel of the merged brick splits it again.
    g.fill_box(Vec3i(8, 8, 8), Vec3i(9, 9, 9), 1.0f);
    TASK_CHECK(!g.get_cell(Vec3i(1, 1, 1)).is_coalesced());

    // Restore the brick, then merge by covering its entire in-domain extent.
    g.fill_box(Vec3i(8, 8, 8), Vec3i(16, 16, 16), 0.0f);
    TASK_CHECK(g.get_cell(Vec3i(1, 1, 1)).is_coalesced());
    TASK_CHECK(g[Vec3i(8, 8, 8)] == 0.0f);
    return 0;
}

int case_fill_box_partial_tail()
{
    // 11 x 9 x 10: the last brick on each axis is partial (3, 1, 2 voxels).
    const Vec3i dims(11, 9, 10);
    HGrid<float> g(dims, -1.0f);

    TASK_CHECK(g.get_top_dims() == Vec3i(2, 2, 2));
    TASK_CHECK(g.get_cell(Vec3i(1, 1, 1)).is_coalesced());

    // Fill exactly the in-domain extent of the partial last brick.
    g.fill_box(Vec3i(8, 8, 8), Vec3i(11, 9, 10), 6.0f);
    TASK_CHECK(g.get_cell(Vec3i(1, 1, 1)).is_coalesced());
    TASK_CHECK(g[Vec3i(8, 8, 8)] == 6.0f);
    TASK_CHECK(g[Vec3i(10, 8, 9)] == 6.0f);
    TASK_CHECK(g[Vec3i(7, 8, 8)] == -1.0f);

    // Split by an edit, then cover the in-domain extent again.
    g.store(Vec3i(10, 8, 9), 8.0f);
    TASK_CHECK(!g.get_cell(Vec3i(1, 1, 1)).is_coalesced());
    TASK_CHECK(g[Vec3i(10, 8, 9)] == 8.0f);
    g.fill_box(Vec3i(8, 8, 8), Vec3i(11, 9, 10), 6.0f);
    TASK_CHECK(g.get_cell(Vec3i(1, 1, 1)).is_coalesced());
    TASK_CHECK(g[Vec3i(10, 8, 9)] == 6.0f);

    // A true partial box inside the tail brick stays split and preserves
    // voxels outside the box.
    g.fill_box(Vec3i(9, 8, 9), Vec3i(10, 9, 10), 2.0f);
    TASK_CHECK(!g.get_cell(Vec3i(1, 1, 1)).is_coalesced());
    TASK_CHECK(g[Vec3i(9, 8, 9)] == 2.0f);
    TASK_CHECK(g[Vec3i(8, 8, 8)] == 6.0f);
    TASK_CHECK(g[Vec3i(10, 8, 8)] == 6.0f);

    // A box clipped to nothing in the last brick does not alter it.
    g.fill_box(Vec3i(11, 9, 10), Vec3i(20, 20, 20), 42.0f);
    TASK_CHECK(g[Vec3i(10, 8, 9)] == 6.0f);
    return 0;
}

int case_edits_ops_and_copies()
{
    const Vec3i dims(9, 10, 11);
    HGrid<float> g(dims, -1.0f);
    g.fill_box(Vec3i(2, 2, 2), Vec3i(7, 8, 9), 3.0f);

    // Copy construction before further edits.
    HGrid<float> copy = g;
    TASK_CHECK(copy[Vec3i(2, 2, 2)] == 3.0f);
    TASK_CHECK(copy[Vec3i(0, 0, 0)] == -1.0f);

    // Editing the copy must not touch the original.
    copy.fill_box(Vec3i(0, 0, 0), Vec3i(9, 10, 11), 5.0f);
    TASK_CHECK(copy[Vec3i(0, 0, 0)] == 5.0f);
    TASK_CHECK(g[Vec3i(0, 0, 0)] == -1.0f);
    TASK_CHECK(g[Vec3i(2, 2, 2)] == 3.0f);

    copy.store(Vec3i(0, 0, 0), 9.0f);
    TASK_CHECK(copy[Vec3i(0, 0, 0)] == 9.0f);
    TASK_CHECK(g[Vec3i(0, 0, 0)] == -1.0f);

    // Editing the original must not touch the existing copy either.
    g.fill_box(Vec3i(8, 9, 10), Vec3i(9, 10, 11), 4.0f);
    TASK_CHECK(g[Vec3i(8, 9, 10)] == 4.0f);
    TASK_CHECK(copy[Vec3i(8, 9, 10)] == 5.0f);

    // store / get_cell / clear still behave as before.
    g.store(Vec3i(4, 4, 4), 11.0f);
    TASK_CHECK(g[Vec3i(4, 4, 4)] == 11.0f);
    TASK_CHECK(g.get_cell(Vec3i(0, 0, 0))[Vec3i(0, 0, 0)] == -1.0f);

    g.clear();
    TASK_CHECK(g[Vec3i(4, 4, 4)] == -1.0f);
    TASK_CHECK(g[Vec3i(8, 9, 10)] == -1.0f);
    for (int i = 0; i < 8; ++i)
        TASK_CHECK(g.get_cell(i).is_coalesced());

    // HGrid remains usable with ordinary non-float scalar types.
    HGrid<int> gi(Vec3i(9, 10, 11), -1);
    gi.fill_box(Vec3i(1, 2, 3), Vec3i(5, 6, 7), 8);
    TASK_CHECK(gi[Vec3i(1, 2, 3)] == 8);
    TASK_CHECK(gi[Vec3i(4, 5, 6)] == 8);
    TASK_CHECK(gi[Vec3i(5, 6, 7)] == -1);
    TASK_CHECK(gi[Vec3i(0, 0, 0)] == -1);
    // Whole-brick fill merges directly for integral types too.
    gi.fill_box(Vec3i(0, 0, 0), Vec3i(8, 8, 8), 2);
    TASK_CHECK(gi.get_cell(Vec3i(0, 0, 0)).is_coalesced());
    TASK_CHECK(gi[Vec3i(7, 7, 7)] == 2);

    HGrid<double> gd(Vec3i(5, 6, 7), -2.0);
    gd.fill_box(Vec3i(1, 1, 1), Vec3i(4, 5, 6), 0.5);
    TASK_CHECK(gd[Vec3i(1, 1, 1)] == 0.5);
    TASK_CHECK(gd[Vec3i(4, 5, 6)] == -2.0);
    return 0;
}

// ---------------------------------------------------------------------------
// surface group
// ---------------------------------------------------------------------------

int case_surface_matches_dense_reference()
{
    const Vec3i dims(18, 15, 12);
    RGrid<float> dense = make_rgrid(dims, field_value);
    HGrid<float> sparse = make_hgrid(dims, field_value);

    // Non-unit scale and offset.
    const XForm xf(Vec3d(-1.0, -2.0, 0.5), Vec3d(3.0, 2.5, 2.0), dims, 0.1);

    const float taus[] = {-5.0f, 0.0f, 4.5f};
    int combos = 0;
    for (float tau : taus)
        for (bool tri : {false, true})
            for (bool high : {true, false})
                for (bool dual : {false, true})
                {
                    Manifold md = volume_polygonize(xf, dense, tau, tri, high, dual);
                    Manifold ms = volume_polygonize(xf, sparse, tau, tri, high, dual);
                    TASK_CHECK(valid(md));
                    TASK_CHECK(valid(ms));
                    TASK_CHECK(ms.no_faces() > 0);
                    TASK_CHECK(same_oriented_surface(md, ms, 1e-6));
                    ++combos;
                }
    TASK_CHECK(combos == 24);
    return 0;
}

int case_surface_nan_holes_and_fill_box_edits()
{
    // Same field, but built with fill_box where possible and single voxel
    // edits inside / outside the filled region. This exercises the sparse
    // path over a mixture of merged and split bricks.
    const Vec3i dims(19, 11, 10);
    HGrid<float> sparse(dims, -1.0f);
    sparse.fill_box(Vec3i(3, 2, 1), Vec3i(15, 9, 8), 2.0f);
    sparse.store(Vec3i(8, 4, 4), -5.0f);
    sparse.store(Vec3i(17, 1, 9), 3.0f);
    sparse.store(Vec3i(2, 2, 1), std::nanf(""));

    RGrid<float> dense(dims, -1.0f);
    for (int z = 0; z < dims[2]; ++z)
        for (int y = 0; y < dims[1]; ++y)
            for (int x = 0; x < dims[0]; ++x)
                if (x >= 3 && x < 15 && y >= 2 && y < 9 && z >= 1 && z < 8)
                    dense.store(Vec3i(x, y, z), 2.0f);
    // Point edits are applied after the box, matching the sparse order.
    dense.store(Vec3i(8, 4, 4), -5.0f);
    dense.store(Vec3i(17, 1, 9), 3.0f);
    dense.store(Vec3i(2, 2, 1), std::nanf(""));

    const XForm xf(dims, 2.0, Vec3d(0.5, 0.25, -0.5));
    for (bool tri : {false, true})
        for (bool high : {true, false})
            for (bool dual : {false, true})
            {
                Manifold md = volume_polygonize(xf, dense, 0.0f, tri, high, dual);
                Manifold ms = volume_polygonize(xf, sparse, 0.0f, tri, high, dual);
                TASK_CHECK(valid(ms));
                TASK_CHECK(ms.no_faces() > 0);
                TASK_CHECK(same_oriented_surface(md, ms, 1e-6));
            }

    // The NaN voxel must produce a hole, i.e. a mesh with boundary edges.
    Manifold with_hole = volume_polygonize(xf, sparse, 0.0f, false, true, false);
    TASK_CHECK(!mesh_is_closed(with_hole));

    // Without the NaN, the filled box is closed.
    sparse.store(Vec3i(2, 2, 1), 2.0f);
    Manifold closed = volume_polygonize(xf, sparse, 0.0f, false, true, false);
    TASK_CHECK(mesh_is_closed(closed));
    return 0;
}

int case_dense_entry_unchanged()
{
    // Expected counts of the pre-existing RGrid entry point, as printed by
    // task_support/baseline: one inside voxel in a 7^3 grid with default
    // outside value -1 and tau = 0.
    RGrid<float> g(Vec3i(7), -1.0f);
    g.store(Vec3i(3, 3, 3), 1.0f);
    XForm xf(Vec3i(7));

    Manifold m_quad = volume_polygonize(xf, g, 0.0f, false, true, false);
    TASK_CHECK(m_quad.no_vertices() == 6);
    TASK_CHECK(m_quad.no_faces() == 8);
    TASK_CHECK(valid(m_quad));

    Manifold m_dual = volume_polygonize(xf, g, 0.0f, false, true, true);
    TASK_CHECK(m_dual.no_vertices() == 8);
    TASK_CHECK(m_dual.no_faces() == 6);
    TASK_CHECK(valid(m_dual));
    return 0;
}

// ---------------------------------------------------------------------------
// sparse_work group
// ---------------------------------------------------------------------------

int case_uniform_large_complete_surface()
{
    const int N = 96;
    const Vec3i dims(N, N, N);
    HGrid<float> sparse(dims, 1.0f);
    SparsePolygonizeStats stats;
    const XForm xf(dims);

    // Defaults: tau = 0, triangles, high values inside, marching cubes
    // connectivity.
    Manifold ms = volume_polygonize(xf, sparse, 0.0f, true, true, false, &stats);

    // Every uniform brick is visited only through its representative, and a
    // brick face is 8x8 voxels. The six boundary slabs of the 96^3 grid have
    // 6*96^2 voxels in total, which is the hard upper bound from the task.
    TASK_CHECK(stats.visited_voxels <= size_t(6) * N * N);
    // The actual count is the union of the six slabs (edges/corners shared).
    TASK_CHECK(stats.visited_voxels == size_t(6) * N * N - size_t(12) * N + 8);
    // One representative read per 8^3 brick; no voxel-by-voxel reads of the
    // uniform interior.
    const size_t bricks = size_t(N / 8) * (N / 8) * (N / 8);
    TASK_CHECK(stats.sample_reads == bricks);
    TASK_CHECK(stats.sample_reads < size_t(N) * N * N);

    // The complete outer surface must be present: a closed mesh spanning the
    // full grid.
    TASK_CHECK(valid(ms));
    TASK_CHECK(mesh_is_closed(ms));
    TASK_CHECK(ms.no_vertices() > 0);
    TASK_CHECK(ms.no_faces() > 0);
    Vec3d lo = mesh_min(ms);
    Vec3d hi = mesh_max(ms);
    for (int i = 0; i < 3; ++i) {
        TASK_CHECK(std::fabs(lo[i] - 0.0) <= 1e-6);
        TASK_CHECK(std::fabs(hi[i] - double(N - 1)) <= 1e-6);
    }

    // The same scalar field through the dense entry point gives the same
    // oriented surface. This proves the bound is met by skipping uniform
    // interior work, not by skipping output.
    RGrid<float> dense(dims, 1.0f);
    Manifold md = volume_polygonize(xf, dense, 0.0f, true, true, false);
    TASK_CHECK(md.no_faces() == ms.no_faces());
    TASK_CHECK(same_oriented_surface(md, ms, 1e-6));

    // The quad/dual variant must also match and respect the same bound.
    SparsePolygonizeStats stats2;
    Manifold ms2 = volume_polygonize(xf, sparse, 0.0f, false, true, true, &stats2);
    Manifold md2 = volume_polygonize(xf, dense, 0.0f, false, true, true);
    TASK_CHECK(stats2.visited_voxels <= size_t(6) * N * N);
    TASK_CHECK(stats2.sample_reads == bricks);
    TASK_CHECK(mesh_is_closed(ms2));
    TASK_CHECK(same_oriented_surface(md2, ms2, 1e-6));
    return 0;
}

int case_stats_reset_and_empty()
{
    const Vec3i dims(32, 32, 32);
    const size_t bricks = size_t(4) * 4 * 4;
    const XForm xf(dims);
    SparsePolygonizeStats stats;

    // An all-outside uniform grid produces no surface and no per-voxel work.
    HGrid<float> outside(dims, -1.0f);
    Manifold m0 = volume_polygonize(xf, outside, 0.0f, true, true, false, &stats);
    TASK_CHECK(m0.no_faces() == 0);
    TASK_CHECK(stats.visited_voxels == 0);
    TASK_CHECK(stats.sample_reads == bricks);

    // Reusing the same stats object must reset the counters, not accumulate.
    HGrid<float> inside(dims, 1.0f);
    Manifold m1 = volume_polygonize(xf, inside, 0.0f, true, true, false, &stats);
    TASK_CHECK(m1.no_faces() > 0);
    TASK_CHECK(stats.visited_voxels > 0);
    TASK_CHECK(stats.visited_voxels <= size_t(6) * 32 * 32);
    TASK_CHECK(stats.sample_reads == bricks);

    Manifold m2 = volume_polygonize(xf, outside, 0.0f, true, true, false, &stats);
    TASK_CHECK(m2.no_faces() == 0);
    TASK_CHECK(stats.visited_voxels == 0);
    TASK_CHECK(stats.sample_reads == bricks);

    // Passing a null stats pointer is allowed.
    Manifold m3 = volume_polygonize(xf, inside, 0.0f, true, true, false, nullptr);
    TASK_CHECK(m3.no_faces() == m1.no_faces());
    return 0;
}

int case_sparse_skips_uniform_interior()
{
    const Vec3i dims(64, 64, 64);
    const Vec3i lo(24, 24, 24);
    const Vec3i hi(40, 40, 40);

    // Only a 16^3 solid is filled; everything else is uniform outside. The
    // filled region aligns with top level bricks, so those bricks are merged.
    HGrid<float> sparse(dims, -1.0f);
    sparse.fill_box(lo, hi, 1.0f);
    RGrid<float> dense(dims, -1.0f);
    for (int z = lo[2]; z < hi[2]; ++z)
        for (int y = lo[1]; y < hi[1]; ++y)
            for (int x = lo[0]; x < hi[0]; ++x)
                dense.store(Vec3i(x, y, z), 1.0f);

    SparsePolygonizeStats stats;
    const XForm xf(dims);
    Manifold ms = volume_polygonize(xf, sparse, 0.0f, false, true, false, &stats);
    Manifold md = volume_polygonize(xf, dense, 0.0f, false, true, false);

    TASK_CHECK(valid(ms));
    TASK_CHECK(same_oriented_surface(md, ms, 1e-6));

    // Only the boundary voxels of the solid are examined; the 16^3 brick
    // interiors and all the empty space are skipped.
    const size_t box_surface = size_t(6) * 16 * 16 - size_t(12) * 16 + 8;
    TASK_CHECK(stats.visited_voxels == box_surface);
    TASK_CHECK(stats.visited_voxels < size_t(64) * 64 * 64 / 100);
    TASK_CHECK(stats.sample_reads == size_t(8) * 8 * 8); // one per brick

    // A single point edit forces only its own brick to be traversed per
    // voxel, not the whole grid.
    sparse.store(Vec3i(1, 1, 1), 1.0f);
    SparsePolygonizeStats stats2;
    Manifold ms2 = volume_polygonize(xf, sparse, 0.0f, false, true, false, &stats2);
    TASK_CHECK(stats2.visited_voxels <= box_surface + 8 * 8 * 8);
    TASK_CHECK(stats2.visited_voxels > box_surface);
    return 0;
}

// ---------------------------------------------------------------------------
// Case table (consumed by task_support/driver.c)
// ---------------------------------------------------------------------------

} // namespace

const struct task_case task_cases[] = {
    {"edits", "fill_box_arrays", case_fill_box_arrays},
    {"edits", "fill_box_brick_merge", case_fill_box_brick_merge},
    {"edits", "fill_box_partial_tail", case_fill_box_partial_tail},
    {"edits", "edits_ops_and_copies", case_edits_ops_and_copies},
    {"surface", "matches_dense_reference", case_surface_matches_dense_reference},
    {"surface", "nan_holes_and_fill_box_edits", case_surface_nan_holes_and_fill_box_edits},
    {"surface", "dense_entry_unchanged", case_dense_entry_unchanged},
    {"sparse_work", "uniform_large_complete_surface", case_uniform_large_complete_surface},
    {"sparse_work", "stats_reset_and_empty", case_stats_reset_and_empty},
    {"sparse_work", "sparse_skips_uniform_interior", case_sparse_skips_uniform_interior},
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
