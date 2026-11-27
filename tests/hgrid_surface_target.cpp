// Regression tests for HGrid::fill_box and the sparse (HGrid) overload of
// HMesh::volume_polygonize. Registered through task_cases/task_case_count;
// the driver in task_support/driver.c provides main().

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <vector>

#include <GEL/CGLA/Vec3i.h>
#include <GEL/CGLA/Vec3d.h>
#include <GEL/Geometry/HGrid.h>
#include <GEL/Geometry/RGrid.h>
#include <GEL/Geometry/XForm.h>
#include <GEL/HMesh/Manifold.h>
#include <GEL/HMesh/polygonize.h>

#include "task_support/check.h"

using namespace CGLA;
using namespace Geometry;
using HMesh::Manifold;

namespace {

const float NAN_V = std::numeric_limits<float>::quiet_NaN();

// ---------------------------------------------------------------------------
// Dense reference array used to check HGrid contents voxel by voxel.
// ---------------------------------------------------------------------------
struct RefGrid {
    Vec3i dims;
    std::vector<float> vox;
    RefGrid(const Vec3i& d, float def) : dims(d), vox(size_t(d[0])*d[1]*d[2], def) {}
    size_t idx(const Vec3i& p) const { return (size_t(p[2])*dims[1]+p[1])*dims[0]+p[0]; }
    float  at(const Vec3i& p) const { return vox[idx(p)]; }
    float& at(const Vec3i& p)       { return vox[idx(p)]; }
    void fill(const Vec3i& lo, const Vec3i& hi, float val) {
        Vec3i a = v_max(lo, Vec3i(0)), b = v_min(hi, dims);
        for (Vec3i p(a); p[2]<b[2]; ++p[2])
            for (p[1]=a[1]; p[1]<b[1]; ++p[1])
                for (p[0]=a[0]; p[0]<b[0]; ++p[0])
                    at(p) = val;
    }
};

bool hgrid_matches(const HGrid<float>& g, const RefGrid& ref) {
    const Vec3i d = g.get_dims();
    for (Vec3i p(0); p[2]<d[2]; ++p[2])
        for (p[1]=0; p[1]<d[1]; ++p[1])
            for (p[0]=0; p[0]<d[0]; ++p[0]) {
                float a = g[p], b = ref.at(p);
                if (std::isnan(a) && std::isnan(b)) continue;
                if (a != b) return false;
            }
    return true;
}

// ---------------------------------------------------------------------------
// Order independent mesh comparison: vertex positions and face connectivity
// (expressed through sorted vertex positions) must agree within tolerance.
// ---------------------------------------------------------------------------
using Pt = std::array<double,3>;

std::vector<Pt> mesh_points(const Manifold& m) {
    std::vector<Pt> pts;
    for (auto v : m.vertices()) {
        const Manifold::Vec& p = m.pos(v);
        pts.push_back({p[0], p[1], p[2]});
    }
    std::sort(pts.begin(), pts.end());
    return pts;
}

std::vector<std::vector<Pt>> mesh_faces(const Manifold& m) {
    std::vector<std::vector<Pt>> fs;
    for (auto f : m.faces()) {
        std::vector<Pt> poly;
        for (auto v : m.incident_vertices(f)) {
            const Manifold::Vec& p = m.pos(v);
            poly.push_back({p[0], p[1], p[2]});
        }
        std::sort(poly.begin(), poly.end());
        fs.push_back(std::move(poly));
    }
    std::sort(fs.begin(), fs.end());
    return fs;
}

bool pts_close(const Pt& a, const Pt& b, double tol) {
    return std::abs(a[0]-b[0]) <= tol &&
           std::abs(a[1]-b[1]) <= tol &&
           std::abs(a[2]-b[2]) <= tol;
}

bool same_geometry(const Manifold& a, const Manifold& b, double tol=1e-6) {
    if (a.no_vertices() != b.no_vertices() || a.no_faces() != b.no_faces())
        return false;
    std::vector<Pt> pa = mesh_points(a), pb = mesh_points(b);
    for (size_t i=0; i<pa.size(); ++i)
        if (!pts_close(pa[i], pb[i], tol)) return false;
    auto fa = mesh_faces(a), fb = mesh_faces(b);
    for (size_t i=0; i<fa.size(); ++i) {
        if (fa[i].size() != fb[i].size()) return false;
        for (size_t j=0; j<fa[i].size(); ++j)
            if (!pts_close(fa[i][j], fb[i][j], tol)) return false;
    }
    return true;
}

// Per-voxel box fill for grids that have no fill_box (used to mirror the
// HGrid::fill_box edits on the dense RGrid reference).
template <class G>
void fill_box_voxels(G& g, const Vec3i& lo, const Vec3i& hi, float val) {
    Vec3i a = v_max(lo, Vec3i(0)), b = v_min(hi, g.get_dims());
    for (Vec3i p(a); p[2]<b[2]; ++p[2])
        for (p[1]=a[1]; p[1]<b[1]; ++p[1])
            for (p[0]=a[0]; p[0]<b[0]; ++p[0])
                g.store(p, val);
}

// Asymmetric scalar field crossing brick boundaries: a constant block, a
// slanted ramp slab (written afterwards, so it wins where they overlap) and
// two single voxel edits. The same construction is mirrored on HGrid and
// RGrid.
float ramp_value(const Vec3i& p) {
    return 0.1f*(p[0]-16) + 0.03f*p[1] - 0.2f;
}

template <class G>
void build_mixed_field(G& g) {
    const Vec3i dims = g.get_dims();
    fill_box_voxels(g, Vec3i(2,3,4), Vec3i(14,15,16), 0.8f);
    for (Vec3i p(10,0,0); p[2]<dims[2]; ++p[2])
        for (p[1]=0; p[1]<dims[1]; ++p[1])
            for (p[0]=10; p[0]<dims[0]; ++p[0])
                g.store(p, ramp_value(p));
    g.store(Vec3i(5,5,5), 2.0f);
    g.store(Vec3i(18,10,10), -3.0f);
}

// HGrid version that uses fill_box for the constant block (dogfooding the
// region edit being tested).
void build_mixed_field_sparse(HGrid<float>& g) {
    const Vec3i dims = g.get_dims();
    g.fill_box(Vec3i(2,3,4), Vec3i(14,15,16), 0.8f);
    for (Vec3i p(10,0,0); p[2]<dims[2]; ++p[2])
        for (p[1]=0; p[1]<dims[1]; ++p[1])
            for (p[0]=10; p[0]<dims[0]; ++p[0])
                g.store(p, ramp_value(p));
    g.store(Vec3i(5,5,5), 2.0f);
    g.store(Vec3i(18,10,10), -3.0f);
}

// ---------------------------------------------------------------------------
// edits group: fill_box behaviour
// ---------------------------------------------------------------------------

int case_fill_box_overlap_bricks() {
    const Vec3i dims(20,18,22);
    HGrid<float> g(dims, -1.5f);
    RefGrid ref(dims, -1.5f);

    // Box fully covering brick (1,1,1) and nothing else of that brick's
    // neighbourhood: the brick must coalesce without expanding.
    g.fill_box(Vec3i(8,8,8), Vec3i(16,16,16), 2.0f);
    ref.fill(Vec3i(8,8,8), Vec3i(16,16,16), 2.0f);
    TASK_CHECK(g.get_cell(Vec3i(1,1,1)).is_coalesced());
    TASK_CHECK(g.get_cell(Vec3i(1,1,1))[Vec3i(9,9,9)] == 2.0f);
    TASK_CHECK(hgrid_matches(g, ref));

    // Overlapping box that partially covers several bricks, including the
    // previously coalesced one.
    g.fill_box(Vec3i(3,4,5), Vec3i(11,12,13), 7.0f);
    ref.fill(Vec3i(3,4,5), Vec3i(11,12,13), 7.0f);
    TASK_CHECK(hgrid_matches(g, ref));
    TASK_CHECK(!g.get_cell(Vec3i(1,1,1)).is_coalesced());
    // The box reaches into brick (0,0,0) as well, so it is expanded too,
    // while brick (0,0,2) is untouched and stays compact.
    TASK_CHECK(!g.get_cell(Vec3i(0,0,0)).is_coalesced());
    TASK_CHECK(g.get_cell(Vec3i(0,0,2)).is_coalesced());
    TASK_CHECK(g[Vec3i(0,0,16)] == -1.5f);             // untouched brick value
    TASK_CHECK(g[Vec3i(15,15,15)] == 2.0f);              // outside the second box
    TASK_CHECK(g[Vec3i(10,10,10)] == 7.0f);              // overwritten by the second box
    TASK_CHECK(g[Vec3i(3,4,5)] == 7.0f);                 // box corner
    TASK_CHECK(g[Vec3i(2,2,2)] == -1.5f);                // default preserved outside
    return 0;
}

int case_fill_box_clipping_empty() {
    const Vec3i dims(10,10,10);
    HGrid<float> g(dims, 0.0f);
    RefGrid ref(dims, 0.0f);

    // Box reaching outside the domain on all sides is clipped.
    g.fill_box(Vec3i(-5,-4,-3), Vec3i(4,5,6), 3.0f);
    ref.fill(Vec3i(-5,-4,-3), Vec3i(4,5,6), 3.0f);
    TASK_CHECK(hgrid_matches(g, ref));
    TASK_CHECK(g[Vec3i(0,0,0)] == 3.0f);
    TASK_CHECK(g[Vec3i(3,4,5)] == 3.0f);
    TASK_CHECK(g[Vec3i(4,5,6)] == 0.0f);

    // Box entirely outside: nothing changes.
    g.fill_box(Vec3i(20,20,20), Vec3i(30,30,30), 9.0f);
    g.fill_box(Vec3i(-30,-30,-30), Vec3i(-10,-10,-10), 9.0f);
    TASK_CHECK(hgrid_matches(g, ref));

    // Empty boxes (lo == hi on some axis) are no-ops.
    g.fill_box(Vec3i(4,4,4), Vec3i(4,8,8), 9.0f);
    g.fill_box(Vec3i(2,2,2), Vec3i(2,2,2), 9.0f);
    TASK_CHECK(hgrid_matches(g, ref));

    // Box covering the whole domain through coordinates beyond the corners.
    g.fill_box(Vec3i(-100), Vec3i(100), 5.0f);
    ref.fill(Vec3i(-100), Vec3i(100), 5.0f);
    TASK_CHECK(hgrid_matches(g, ref));
    for (int i=0; i<8; ++i)
        TASK_CHECK(g.get_cell(i).is_coalesced());
    return 0;
}

int case_fill_box_tail_bricks() {
    const Vec3i dims(9,10,11); // not multiples of the brick size: top dims 2x2x2
    HGrid<float> g(dims, -1.0f);
    RefGrid ref(dims, -1.0f);
    TASK_CHECK(g.get_top_dims() == Vec3i(2,2,2));

    // Cover exactly the valid domain of the tail brick (1,1,1): only
    // 1x2x3 voxels, but that is the whole logical brick as far as the
    // voxel domain is concerned, so it coalesces.
    g.fill_box(Vec3i(8,8,8), Vec3i(9,10,11), 4.0f);
    ref.fill(Vec3i(8,8,8), Vec3i(9,10,11), 4.0f);
    TASK_CHECK(g.get_cell(Vec3i(1,1,1)).is_coalesced());
    TASK_CHECK(hgrid_matches(g, ref));

    // Single voxel inside the tail brick: partial coverage, must expand.
    g.fill_box(Vec3i(8,9,10), Vec3i(9,10,11), 7.0f);
    ref.fill(Vec3i(8,9,10), Vec3i(9,10,11), 7.0f);
    TASK_CHECK(!g.get_cell(Vec3i(1,1,1)).is_coalesced());
    TASK_CHECK(g[Vec3i(8,9,10)] == 7.0f);
    TASK_CHECK(g[Vec3i(8,9,9)] == 4.0f);
    TASK_CHECK(g[Vec3i(8,8,10)] == 4.0f);
    TASK_CHECK(hgrid_matches(g, ref));

    // Fill the whole grid: every brick, tails included, coalesces.
    g.fill_box(Vec3i(0), dims, 6.0f);
    ref.fill(Vec3i(0), dims, 6.0f);
    TASK_CHECK(hgrid_matches(g, ref));
    for (int i=0; i<8; ++i)
        TASK_CHECK(g.get_cell(i).is_coalesced());
    return 0;
}

int case_fill_box_recoalesce() {
    const Vec3i dims(16,16,16);
    HGrid<float> g(dims, 2.0f);
    RefGrid ref(dims, 2.0f);

    // Disturb one brick with a point edit, then cover it completely with
    // the default value: it must merge back into a single value.
    g.store(Vec3i(3,3,3), 9.0f);
    TASK_CHECK(!g.get_cell(Vec3i(0,0,0)).is_coalesced());
    g.fill_box(Vec3i(0,0,0), Vec3i(8,8,8), 2.0f);
    ref.fill(Vec3i(0,0,0), Vec3i(8,8,8), 2.0f);
    TASK_CHECK(g.get_cell(Vec3i(0,0,0)).is_coalesced());
    TASK_CHECK(g[Vec3i(3,3,3)] == 2.0f);
    TASK_CHECK(hgrid_matches(g, ref));

    // Partial coverage that restores uniformity only after a second fill.
    g.store(Vec3i(9,2,2), 5.0f);
    g.store(Vec3i(11,6,6), 5.0f);
    ref.at(Vec3i(9,2,2)) = 5.0f;
    ref.at(Vec3i(11,6,6)) = 5.0f;
    g.fill_box(Vec3i(8,0,0), Vec3i(12,8,8), 5.0f); // half of brick (1,0,0)
    ref.fill(Vec3i(8,0,0), Vec3i(12,8,8), 5.0f);
    TASK_CHECK(!g.get_cell(Vec3i(1,0,0)).is_coalesced()); // 5.0 and 2.0 mixed
    g.fill_box(Vec3i(12,0,0), Vec3i(16,8,8), 5.0f); // the other half
    ref.fill(Vec3i(12,0,0), Vec3i(16,8,8), 5.0f);
    TASK_CHECK(g.get_cell(Vec3i(1,0,0)).is_coalesced());  // uniform again
    TASK_CHECK(g[Vec3i(15,7,7)] == 5.0f);
    TASK_CHECK(hgrid_matches(g, ref));

    // Filling with the value a coalesced brick already holds is a no-op
    // that must not expand the brick.
    g.fill_box(Vec3i(9,1,1), Vec3i(10,2,2), 5.0f);
    TASK_CHECK(g.get_cell(Vec3i(1,0,0)).is_coalesced());
    TASK_CHECK(hgrid_matches(g, ref));
    return 0;
}

int case_fill_box_copy_independent() {
    const Vec3i dims(20,20,20);
    HGrid<float> g(dims, -1.0f);
    g.fill_box(Vec3i(2,2,2), Vec3i(14,14,14), 3.0f);
    g.store(Vec3i(0,0,0), 8.0f);

    HGrid<float> copy = g; // copy construction
    RefGrid snap(dims, -1.0f);
    snap.fill(Vec3i(2,2,2), Vec3i(14,14,14), 3.0f);
    snap.at(Vec3i(0,0,0)) = 8.0f;
    TASK_CHECK(hgrid_matches(copy, snap));

    // Edits to the original leave the copy alone.
    g.store(Vec3i(5,5,5), 42.0f);
    g.fill_box(Vec3i(16,16,16), Vec3i(20,20,20), 9.0f);
    TASK_CHECK(hgrid_matches(copy, snap));
    TASK_CHECK(copy[Vec3i(5,5,5)] == 3.0f);
    TASK_CHECK(copy[Vec3i(18,18,18)] == -1.0f);

    // Clearing the original leaves the copy alone.
    g.clear();
    TASK_CHECK(hgrid_matches(copy, snap));
    for (int i=0; i<27; ++i)
        TASK_CHECK(g.get_cell(i).is_coalesced());
    TASK_CHECK(g[Vec3i(5,5,5)] == -1.0f);

    // Edits to the copy leave the (cleared) original alone.
    copy.fill_box(Vec3i(1,1,1), Vec3i(9,9,9), 4.0f);
    copy.store(Vec3i(19,19,19), 6.0f);
    TASK_CHECK(g[Vec3i(1,1,1)] == -1.0f);
    TASK_CHECK(g[Vec3i(19,19,19)] == -1.0f);
    TASK_CHECK(copy[Vec3i(1,1,1)] == 4.0f);
    TASK_CHECK(copy[Vec3i(19,19,19)] == 6.0f);
    return 0;
}

int case_fill_box_then_store_clear() {
    const Vec3i dims(17,9,13);
    HGrid<float> g(dims, 0.5f);
    RefGrid ref(dims, 0.5f);

    g.fill_box(Vec3i(4,2,3), Vec3i(16,8,12), -2.0f);
    ref.fill(Vec3i(4,2,3), Vec3i(16,8,12), -2.0f);
    TASK_CHECK(hgrid_matches(g, ref));

    // Single point edits after the box fill.
    g.store(Vec3i(5,5,5), 11.0f);
    ref.at(Vec3i(5,5,5)) = 11.0f;
    g.store(Vec3i(0,0,0), -7.0f);
    ref.at(Vec3i(0,0,0)) = -7.0f;
    g.store(Vec3i(16,8,12), 3.0f);
    ref.at(Vec3i(16,8,12)) = 3.0f;
    TASK_CHECK(hgrid_matches(g, ref));

    // A second fill over the edited points.
    g.fill_box(Vec3i(0,0,0), Vec3i(8,8,8), 1.0f);
    ref.fill(Vec3i(0,0,0), Vec3i(8,8,8), 1.0f);
    TASK_CHECK(hgrid_matches(g, ref));
    TASK_CHECK(g.get_cell(Vec3i(0,0,0)).is_coalesced());

    // clear() restores the default value everywhere and re-coalesces.
    g.clear();
    RefGrid cleared(dims, 0.5f);
    TASK_CHECK(hgrid_matches(g, cleared));
    const Vec3i td = g.get_top_dims();
    for (int i=0; i<td[0]*td[1]*td[2]; ++i)
        TASK_CHECK(g.get_cell(i).is_coalesced());
    return 0;
}

int case_fill_box_int_grid() {
    // The HGrid template must keep working for ordinary numeric types.
    HGrid<int> g(Vec3i(10,10,10), 0);
    g.fill_box(Vec3i(2,2,2), Vec3i(9,9,9), 7);
    TASK_CHECK(g[Vec3i(2,2,2)] == 7);
    TASK_CHECK(g[Vec3i(8,8,8)] == 7);
    TASK_CHECK(g[Vec3i(9,9,9)] == 0);
    // Brick (1,1,1) is only partially covered (its valid domain is
    // [8,10)^3), so it must have been expanded.
    TASK_CHECK(!g.get_cell(Vec3i(1,1,1)).is_coalesced());
    g.store(Vec3i(8,8,8), 3);
    TASK_CHECK(g[Vec3i(8,8,8)] == 3);
    g.fill_box(Vec3i(0), Vec3i(10,10,10), -4);
    TASK_CHECK(g[Vec3i(5,5,5)] == -4);
    for (int i=0; i<8; ++i)
        TASK_CHECK(g.get_cell(i).is_coalesced());
    g.clear();
    TASK_CHECK(g[Vec3i(5,5,5)] == 0);
    return 0;
}

// ---------------------------------------------------------------------------
// surface group: sparse and dense polygonize must agree
// ---------------------------------------------------------------------------

int case_surface_dense_golden() {
    // The old RGrid entry point must be unaffected: single inside voxel in a
    // 7^3 grid, same setup as the baseline smoke test.
    RGrid<float> g(Vec3i(7), -1.0f);
    g.store(Vec3i(3,3,3), 1.0f);
    XForm xf(Vec3i(7));
    Manifold mc = HMesh::volume_polygonize(xf, g, 0.0f, false, true, false);
    TASK_CHECK(mc.no_vertices() == 6);
    TASK_CHECK(mc.no_faces() == 8);
    TASK_CHECK(HMesh::valid(mc));
    Manifold mc_tri = HMesh::volume_polygonize(xf, g, 0.0f, true, true, false);
    TASK_CHECK(mc_tri.no_vertices() == 6);
    TASK_CHECK(mc_tri.no_faces() == 8);
    Manifold dual = HMesh::volume_polygonize(xf, g, 0.0f, false, true, true);
    TASK_CHECK(dual.no_vertices() == 8);
    TASK_CHECK(dual.no_faces() == 6);
    TASK_CHECK(HMesh::valid(dual));
    Manifold dual_tri = HMesh::volume_polygonize(xf, g, 0.0f, true, true, true);
    TASK_CHECK(dual_tri.no_vertices() == 8);
    TASK_CHECK(dual_tri.no_faces() == 12);
    TASK_CHECK(HMesh::valid(dual_tri));
    return 0;
}

// Compare sparse vs dense on the mixed field for one option set.
bool check_mixed_field(float tau, bool tris, bool high_inside, bool dual,
                       const XForm& xf, const Vec3i& dims) {
    HGrid<float> hg(dims, -1.0f);
    build_mixed_field_sparse(hg);
    RGrid<float> rg(dims, -1.0f);
    build_mixed_field(rg);
    Manifold ms = HMesh::volume_polygonize(xf, hg, tau, tris, high_inside, dual);
    Manifold md = HMesh::volume_polygonize(xf, rg, tau, tris, high_inside, dual);
    return ms.no_faces() > 0 && HMesh::valid(ms) && same_geometry(ms, md);
}

int case_surface_default_options() {
    const Vec3i dims(22,20,24);
    TASK_CHECK(check_mixed_field(0.0f, true, true, false, XForm(dims), dims));
    return 0;
}

int case_surface_dual_connectivity() {
    const Vec3i dims(22,20,24);
    TASK_CHECK(check_mixed_field(0.0f, true, true, true, XForm(dims), dims));
    return 0;
}

int case_surface_quads_threshold() {
    const Vec3i dims(22,20,24);
    TASK_CHECK(check_mixed_field(0.25f, false, true, false, XForm(dims), dims));
    return 0;
}

int case_surface_low_is_inside() {
    const Vec3i dims(22,20,24);
    TASK_CHECK(check_mixed_field(0.0f, true, false, false, XForm(dims), dims));
    return 0;
}

int case_surface_nonunit_xform() {
    const Vec3i dims(22,20,24);
    XForm xf(dims, 2.0, Vec3d(0.5,-1.0,2.0));
    TASK_CHECK(check_mixed_field(-0.1f, true, true, false, xf, dims));
    TASK_CHECK(check_mixed_field(0.1f, false, true, true, xf, dims));
    return 0;
}

int case_surface_nan_hole() {
    const Vec3i dims(20,20,20);
    HGrid<float> hg(dims, -1.0f);
    hg.fill_box(Vec3i(2,2,2), Vec3i(18,18,18), 1.0f);
    RGrid<float> rg(dims, -1.0f);
    fill_box_voxels(rg, Vec3i(2,2,2), Vec3i(18,18,18), 1.0f);
    // NaN slab cutting through the block across brick boundaries.
    for (Vec3i p(9,0,0); p[2]<dims[2]; ++p[2])
        for (p[1]=0; p[1]<dims[1]; ++p[1])
            for (p[0]=9; p[0]<11; ++p[0]) {
                hg.store(p, NAN_V);
                rg.store(p, NAN_V);
            }
    XForm xf(dims);
    for (bool dual : {false, true}) {
        Manifold ms = HMesh::volume_polygonize(xf, hg, 0.0f, true, true, dual);
        Manifold md = HMesh::volume_polygonize(xf, rg, 0.0f, true, true, dual);
        TASK_CHECK(ms.no_faces() > 0);
        TASK_CHECK(same_geometry(ms, md));
        TASK_CHECK(HMesh::valid(ms));
    }
    // The NaN slab must remove surface: fewer faces than the uncut block.
    HGrid<float> hg2(dims, -1.0f);
    hg2.fill_box(Vec3i(2,2,2), Vec3i(18,18,18), 1.0f);
    Manifold whole = HMesh::volume_polygonize(xf, hg2, 0.0f, true, true, false);
    Manifold cut = HMesh::volume_polygonize(xf, hg, 0.0f, true, true, false);
    TASK_CHECK(cut.no_faces() < whole.no_faces());
    return 0;
}

int case_surface_nan_surrounded() {
    // Coalesced NaN bricks around a solid block at the domain corner: NaN
    // voxels form missing-sample holes, so only the domain boundary sides
    // of the block produce surface.
    const Vec3i dims(16,16,16);
    HGrid<float> hg(dims, NAN_V);
    hg.fill_box(Vec3i(0,0,0), Vec3i(8,8,8), 1.0f);
    RGrid<float> rg(dims, NAN_V);
    fill_box_voxels(rg, Vec3i(0,0,0), Vec3i(8,8,8), 1.0f);
    XForm xf(dims);
    HMesh::SparsePolygonizeStats st;
    // Dual connectivity keeps every cuberille quad as a face.
    Manifold ms = HMesh::volume_polygonize(xf, hg, 0.0f, false, true, true, &st);
    Manifold md = HMesh::volume_polygonize(xf, rg, 0.0f, false, true, true);
    TASK_CHECK(same_geometry(ms, md));
    TASK_CHECK(ms.no_faces() == 3*64); // only the three domain boundary sides
    TASK_CHECK(st.visited_voxels == 3*64);
    TASK_CHECK(st.sample_reads == 11); // 8 brick values + 3 uniform neighbors
    return 0;
}

int case_surface_uniform_small() {
    // Fully uniform grid: the surface is exactly the domain boundary.
    const Vec3i dims(24,24,24);
    HGrid<float> hg(dims, 1.0f);
    RGrid<float> rg(dims, 1.0f);
    XForm xf(dims);
    for (bool dual : {false, true})
        for (bool tris : {false, true}) {
            Manifold ms = HMesh::volume_polygonize(xf, hg, 0.0f, tris, true, dual);
            Manifold md = HMesh::volume_polygonize(xf, rg, 0.0f, tris, true, dual);
            TASK_CHECK(ms.no_faces() > 0);
            TASK_CHECK(same_geometry(ms, md));
            TASK_CHECK(HMesh::valid(ms));
        }
    return 0;
}

int case_surface_sculpted_block() {
    // Sculpt a solid block with fill_box, then polygonize the sparse data.
    const Vec3i dims(20,22,18);
    HGrid<float> hg(dims, -1.0f);
    hg.fill_box(Vec3i(4,5,3), Vec3i(14,15,13), 2.0f);
    RGrid<float> rg(dims, -1.0f);
    fill_box_voxels(rg, Vec3i(4,5,3), Vec3i(14,15,13), 2.0f);
    XForm xf(dims);
    Manifold ms = HMesh::volume_polygonize(xf, hg, 0.0f, true, true, false);
    Manifold md = HMesh::volume_polygonize(xf, rg, 0.0f, true, true, false);
    TASK_CHECK(ms.no_faces() > 0);
    TASK_CHECK(HMesh::valid(ms));
    TASK_CHECK(same_geometry(ms, md));
    // The block is 10x10x10 voxels: its cuberille surface area is 600 quads
    // before triangulation.
    Manifold quads = HMesh::volume_polygonize(xf, hg, 0.0f, false, true, true);
    TASK_CHECK(quads.no_faces() == 6*100);
    return 0;
}

// ---------------------------------------------------------------------------
// sparse_work group: work tallies of the sparse traversal
// ---------------------------------------------------------------------------

int case_sparse_work_uniform_96() {
    const int N = 96;
    const Vec3i dims(N,N,N);
    HGrid<float> hg(dims, 1.0f);
    XForm xf(dims);

    // Quads + dual connectivity keeps the cuberille combinatorics: the
    // complete surface of a 96^3 uniform grid is 6*96^2 quads and
    // 97^3-95^3 vertices.
    HMesh::SparsePolygonizeStats st;
    Manifold mq = HMesh::volume_polygonize(xf, hg, 0.0f, false, true, true, &st);
    TASK_CHECK(mq.no_faces() == 6*N*N);
    TASK_CHECK(mq.no_vertices() == 97*97*97 - 95*95*95);
    TASK_CHECK(st.visited_voxels <= size_t(6)*N*N); // the required bound
    TASK_CHECK(st.visited_voxels == size_t(6)*N*N); // exact for this traversal
    TASK_CHECK(st.sample_reads == 1728 + 9504);     // brick values + brick face probes
    TASK_CHECK(st.sample_reads < size_t(N)*N*N/8);  // far below dense scanning

    // Marching cubes connectivity, quads: one vertex per cuberille quad,
    // one face per cuberille vertex.
    HMesh::SparsePolygonizeStats st2;
    Manifold mmc = HMesh::volume_polygonize(xf, hg, 0.0f, false, true, false, &st2);
    TASK_CHECK(mmc.no_vertices() == 6*N*N);
    TASK_CHECK(mmc.no_faces() == 97*97*97 - 95*95*95);
    TASK_CHECK(st2.visited_voxels == st.visited_voxels);
    TASK_CHECK(st2.sample_reads == st.sample_reads);

    // Default options: compare against the dense entry point on the same field.
    RGrid<float> rg(dims, 1.0f);
    Manifold ms = HMesh::volume_polygonize(xf, hg, 0.0f, true, true, false);
    Manifold md = HMesh::volume_polygonize(xf, rg, 0.0f, true, true, false);
    TASK_CHECK(ms.no_faces() > 0);
    TASK_CHECK(same_geometry(ms, md));
    TASK_CHECK(HMesh::valid(ms));
    return 0;
}

int case_sparse_work_mixed_field() {
    const Vec3i dims(40,40,40);
    HGrid<float> hg(dims, -1.0f);
    build_mixed_field_sparse(hg);
    RGrid<float> rg(dims, -1.0f);
    build_mixed_field(rg);
    XForm xf(dims);

    HMesh::SparsePolygonizeStats st;
    Manifold ms = HMesh::volume_polygonize(xf, hg, 0.0f, true, true, false, &st);
    const size_t nvox = size_t(dims[0])*dims[1]*dims[2];
    TASK_CHECK(st.visited_voxels > 0);
    TASK_CHECK(st.visited_voxels <= 3*nvox); // face voxels can be revisited per direction
    TASK_CHECK(st.sample_reads > 0);
    TASK_CHECK(st.sample_reads <= 8*nvox);   // below per-voxel dense probing
    TASK_CHECK(HMesh::valid(ms));

    // Counters are reset on every call: a second identical call reproduces
    // exactly the same tallies.
    HMesh::SparsePolygonizeStats st_again;
    HMesh::volume_polygonize(xf, hg, 0.0f, true, true, false, &st_again);
    TASK_CHECK(st_again.visited_voxels == st.visited_voxels);
    TASK_CHECK(st_again.sample_reads == st.sample_reads);

    // Null stats pointer is accepted and yields the same surface.
    Manifold ms2 = HMesh::volume_polygonize(xf, hg, 0.0f, true, true, false, nullptr);
    TASK_CHECK(same_geometry(ms, ms2));

    // Same field through the dense entry point: same surface.
    Manifold md = HMesh::volume_polygonize(xf, rg, 0.0f, true, true, false);
    TASK_CHECK(same_geometry(ms, md));
    return 0;
}

int case_sparse_work_nan_uniform() {
    // A solid block entirely surrounded by coalesced NaN bricks: NaN means
    // missing samples, so no surface is generated at all and no voxel needs
    // to be visited.
    const Vec3i dims(48,48,48);
    HGrid<float> hg(dims, NAN_V);
    hg.fill_box(Vec3i(8,8,8), Vec3i(40,40,40), 1.0f);
    XForm xf(dims);
    HMesh::SparsePolygonizeStats st;
    Manifold ms = HMesh::volume_polygonize(xf, hg, 0.0f, true, true, false, &st);
    TASK_CHECK(ms.no_vertices() == 0);
    TASK_CHECK(ms.no_faces() == 0);
    TASK_CHECK(st.visited_voxels == 0);
    TASK_CHECK(st.sample_reads == 216 + 384); // 6^3 brick values + 4^3*6 neighbor probes

    RGrid<float> rg(dims, NAN_V);
    fill_box_voxels(rg, Vec3i(8,8,8), Vec3i(40,40,40), 1.0f);
    Manifold md = HMesh::volume_polygonize(xf, rg, 0.0f, true, true, false);
    TASK_CHECK(md.no_faces() == 0);
    TASK_CHECK(same_geometry(ms, md));
    return 0;
}

int case_sparse_work_outside_default() {
    // Uniformly outside grid: no surface, no visited voxels, one
    // representative read per brick.
    const Vec3i dims(96,96,96);
    HGrid<float> hg(dims, -1.0f);
    XForm xf(dims);
    HMesh::SparsePolygonizeStats st;
    Manifold ms = HMesh::volume_polygonize(xf, hg, 0.0f, true, true, false, &st);
    TASK_CHECK(ms.no_vertices() == 0);
    TASK_CHECK(ms.no_faces() == 0);
    TASK_CHECK(st.visited_voxels == 0);
    TASK_CHECK(st.sample_reads == 12*12*12);
    return 0;
}

} // namespace

const task_case task_cases[] = {
    {"edits", "fill_box_overlap_bricks", case_fill_box_overlap_bricks},
    {"edits", "fill_box_clipping_empty", case_fill_box_clipping_empty},
    {"edits", "fill_box_tail_bricks", case_fill_box_tail_bricks},
    {"edits", "fill_box_recoalesce", case_fill_box_recoalesce},
    {"edits", "fill_box_copy_independent", case_fill_box_copy_independent},
    {"edits", "fill_box_then_store_clear", case_fill_box_then_store_clear},
    {"edits", "fill_box_int_grid", case_fill_box_int_grid},
    {"surface", "dense_golden", case_surface_dense_golden},
    {"surface", "default_options", case_surface_default_options},
    {"surface", "dual_connectivity", case_surface_dual_connectivity},
    {"surface", "quads_threshold", case_surface_quads_threshold},
    {"surface", "low_is_inside", case_surface_low_is_inside},
    {"surface", "nonunit_xform", case_surface_nonunit_xform},
    {"surface", "nan_hole", case_surface_nan_hole},
    {"surface", "nan_surrounded", case_surface_nan_surrounded},
    {"surface", "uniform_small", case_surface_uniform_small},
    {"surface", "sculpted_block", case_surface_sculpted_block},
    {"sparse_work", "uniform_96", case_sparse_work_uniform_96},
    {"sparse_work", "mixed_field", case_sparse_work_mixed_field},
    {"sparse_work", "nan_uniform", case_sparse_work_nan_uniform},
    {"sparse_work", "outside_default", case_sparse_work_outside_default},
};

const size_t task_case_count = sizeof(task_cases)/sizeof(task_cases[0]);
