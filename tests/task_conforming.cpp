// Conforming greedy mesher task tests.
//
// This file never defines OGT_VOXEL_MESHIFY_IMPLEMENTATION (the implementation is linked in separately) and
// never calls any split/triangulation helper of the new path -- those are file-local anyway. Every check below
// is an independent reference built only from the raw voxel input and the public ogt_mesh output.

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <vector>
#include <map>
#include <set>
#include <utility>

#include "task_support/check.h"
#include "src/ogt_voxel_meshify.h"

namespace {

// ---------------------------------------------------------------------------
// shared reference helpers
// ---------------------------------------------------------------------------

struct Grid {
    int nx, ny, nz;
    std::vector<uint8_t> v;
    Grid(int x, int y, int z) : nx(x), ny(y), nz(z), v((size_t)x * y * z, 0) {}
    uint8_t at(int x, int y, int z) const {
        if (x < 0 || y < 0 || z < 0 || x >= nx || y >= ny || z >= nz)
            return 0; // out of bounds is empty air, matching the mesher's occlusion rule
        return v[(size_t)x + (size_t)y * nx + (size_t)z * nx * ny];
    }
    void set(int x, int y, int z, uint8_t p) { v[(size_t)x + (size_t)y * nx + (size_t)z * nx * ny] = p; }
};

struct Face {
    int x, y, z;   // solid voxel owning the face
    int dir;       // 0=-x 1=+x 2=-y 3=+y 4=-z 5=+z
    uint8_t pal;
};

// independently enumerate every visible unit face straight from the voxel field
std::vector<Face> enumerate_faces(const Grid& g) {
    static const int d[6][3] = { {-1,0,0},{1,0,0},{0,-1,0},{0,1,0},{0,0,-1},{0,0,1} };
    std::vector<Face> out;
    for (int z = 0; z < g.nz; z++)
        for (int y = 0; y < g.ny; y++)
            for (int x = 0; x < g.nx; x++) {
                uint8_t p = g.at(x, y, z);
                if (!p)
                    continue;
                for (int k = 0; k < 6; k++)
                    if (g.at(x + d[k][0], y + d[k][1], z + d[k][2]) == 0)
                        out.push_back(Face{ x, y, z, k, p });
            }
    return out;
}

struct IVec3 {
    int64_t x, y, z;
    bool operator<(const IVec3& o) const {
        if (x != o.x) return x < o.x;
        if (y != o.y) return y < o.y;
        return z < o.z;
    }
    bool operator==(const IVec3& o) const { return x == o.x && y == o.y && z == o.z; }
};

IVec3 vertex_pos(const ogt_mesh& m, uint32_t i) {
    const ogt_mesh_vec3& p = m.vertices[i].pos;
    return IVec3{ (int64_t)p.x, (int64_t)p.y, (int64_t)p.z };
}

// strict 2d half-plane test on integer coordinates, with mesh positions multiplied by scale
bool strict_in_triangle_2d_scaled(int64_t px, int64_t py, const IVec3& a, const IVec3& b, const IVec3& c,
                                  int o1, int o2, int64_t scale) {
    auto coord = [&](const IVec3& q, int axis) {
        return scale * (axis == 0 ? q.x : axis == 1 ? q.y : q.z);
    };
    int64_t ax = coord(a, o1), ay = coord(a, o2);
    int64_t bx = coord(b, o1), by = coord(b, o2);
    int64_t cx = coord(c, o1), cy = coord(c, o2);
    int64_t d1 = (px - bx) * (ay - by) - (py - by) * (ax - bx);
    int64_t d2 = (px - cx) * (by - cy) - (py - cy) * (bx - cx);
    int64_t d3 = (px - ax) * (cy - ay) - (py - ay) * (cx - ax);
    return (d1 < 0 && d2 < 0 && d3 < 0) || (d1 > 0 && d2 > 0 && d3 > 0);
}

// returns 0 and the unique triangle strictly containing the scaled sample point; -1 uncovered; -2 overlapping
int covering_triangle(const ogt_mesh& m, int plane_axis, int64_t plane_value, int o1, int o2,
                      int64_t s1, int64_t s2, int64_t scale, uint32_t& tri_index) {
    int found = -1;
    for (uint32_t t = 0; t < m.index_count; t += 3) {
        IVec3 p[3] = { vertex_pos(m, m.indices[t]), vertex_pos(m, m.indices[t + 1]), vertex_pos(m, m.indices[t + 2]) };
        auto c = [&](const IVec3& q) { return plane_axis == 0 ? q.x : plane_axis == 1 ? q.y : q.z; };
        if (c(p[0]) != plane_value || c(p[1]) != plane_value || c(p[2]) != plane_value)
            continue;
        if (strict_in_triangle_2d_scaled(s1, s2, p[0], p[1], p[2], o1, o2, scale)) {
            if (found >= 0)
                return -2; // overlapping coverage
            found = (int)t;
        }
    }
    if (found < 0)
        return -1; // sample landed on an edge or in a gap
    tri_index = (uint32_t)found;
    return 0;
}

// fixed, deterministic jitter table -- no RNG so results are repeatable. Generic, non-collinear offsets so that
// for any covered unit face at least one sample falls strictly inside a triangle (never all on triangle edges).
static const int64_t kJitter[8][2] = {
    {31, 47}, {59, 23}, {17, 67}, {71, 13}, {43, 37}, {11, 73}, {61, 29}, {37, 53}
};

// exactly one triangle covers every visible unit face, carrying that face's material
bool check_face_coverage(const Grid& g, const ogt_mesh& m) {
    std::vector<Face> faces = enumerate_faces(g);
    const int64_t scale = 100;
    for (size_t fi = 0; fi < faces.size(); fi++) {
        const Face& f = faces[fi];
        int axis = f.dir / 2;
        int sign = (f.dir & 1) ? 1 : -1;
        int64_t plane = (int64_t)(axis == 0 ? (sign > 0 ? f.x + 1 : f.x)
                                   : axis == 1 ? (sign > 0 ? f.y + 1 : f.y)
                                               : (sign > 0 ? f.z + 1 : f.z));
        int o1 = (axis + 1) % 3, o2 = (axis + 2) % 3;
        int cell[3] = { f.x, f.y, f.z };
        bool covered = false;
        for (int j = 0; j < 8; j++) {
            int64_t s1 = (int64_t)cell[o1] * scale + kJitter[j][0];
            int64_t s2 = (int64_t)cell[o2] * scale + kJitter[j][1];
            uint32_t ti = 0;
            int rc = covering_triangle(m, axis, plane, o1, o2, s1, s2, scale, ti);
            if (rc == -1)
                continue; // sample landed on an edge, try another jitter
            if (rc != 0)
                return false;
            const ogt_mesh_vertex& tv = m.vertices[m.indices[ti]];
            if (tv.palette_index != f.pal)
                return false;
            covered = true;
            break;
        }
        if (!covered)
            return false;
    }
    return true;
}

// doubled signed area of a triangle in 3d, projected onto its constant-coordinate plane
int64_t triangle_double_area(const ogt_mesh& m, uint32_t t) {
    IVec3 a = vertex_pos(m, m.indices[t]);
    IVec3 b = vertex_pos(m, m.indices[t + 1]);
    IVec3 c = vertex_pos(m, m.indices[t + 2]);
    int64_t ux = b.x - a.x, uy = b.y - a.y, uz = b.z - a.z;
    int64_t vx = c.x - a.x, vy = c.y - a.y, vz = c.z - a.z;
    int64_t nx = uy * vz - uz * vy, ny = uz * vx - ux * vz, nz = ux * vy - uy * vx;
    int64_t ax = nx < 0 ? -nx : nx, ay = ny < 0 ? -ny : ny, az = nz < 0 ? -nz : nz;
    return ax + ay + az; // exactly one component is non-zero for an axis-aligned planar triangle
}

// all output positions are integer grid coordinates
bool positions_integral(const ogt_mesh& m) {
    for (uint32_t i = 0; i < m.vertex_count; i++) {
        const ogt_mesh_vec3& p = m.vertices[i].pos;
        if (p.x != (float)(int64_t)p.x || p.y != (float)(int64_t)p.y || p.z != (float)(int64_t)p.z)
            return false;
    }
    return true;
}

// every triangle is planar on one axis plane, has positive area and agrees with its stored outward normal
bool triangles_oriented(const ogt_mesh& m) {
    for (uint32_t t = 0; t < m.index_count; t += 3) {
        const ogt_mesh_vertex& va = m.vertices[m.indices[t]];
        const ogt_mesh_vertex& vb = m.vertices[m.indices[t + 1]];
        const ogt_mesh_vertex& vc = m.vertices[m.indices[t + 2]];
        if (triangle_double_area(m, t) <= 0)
            return false;
        if (!(va.pos.x == vb.pos.x && vb.pos.x == vc.pos.x) &&
            !(va.pos.y == vb.pos.y && vb.pos.y == vc.pos.y) &&
            !(va.pos.z == vb.pos.z && vb.pos.z == vc.pos.z))
            return false;
        double ux = vb.pos.x - va.pos.x, uy = vb.pos.y - va.pos.y, uz = vb.pos.z - va.pos.z;
        double vx = vc.pos.x - va.pos.x, vy = vc.pos.y - va.pos.y, vz = vc.pos.z - va.pos.z;
        double gx = uy * vz - uz * vy, gy = uz * vx - ux * vz, gz = ux * vy - uy * vx;
        if (gx * va.normal.x + gy * va.normal.y + gz * va.normal.z <= 0.0)
            return false;
        // face attributes are uniform per triangle and never averaged
        if (va.normal.x != vb.normal.x || va.normal.y != vb.normal.y || va.normal.z != vb.normal.z)
            return false;
        if (vb.normal.x != vc.normal.x || vb.normal.y != vc.normal.y || vb.normal.z != vc.normal.z)
            return false;
        if (va.palette_index != vb.palette_index || vb.palette_index != vc.palette_index)
            return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// independent greedy-rectangle reference: the quads emitted by the existing
// public greedy mesher are exactly the "original greedy rectangles" the new
// path must keep and conform. No new-path split/triangulation helper is used.
// ---------------------------------------------------------------------------

struct Rect {
    int axis;            // 0=x 1=y 2=z
    int plane;           // fixed coordinate of the face plane
    int axes[2];         // the two in-plane axes, sorted
    int lo[2], hi[2];    // inclusive coordinate bounds on axes[0]/axes[1]
    uint8_t pal;
    int dir;
};

int normal_dir(const ogt_mesh_vec3& n) {
    if (n.x < 0.0f) return 0;
    if (n.x > 0.0f) return 1;
    if (n.y < 0.0f) return 2;
    if (n.y > 0.0f) return 3;
    if (n.z < 0.0f) return 4;
    return 5;
}

// extract every greedy rectangle (quad) straight from the public greedy mesh
std::vector<Rect> greedy_rectangles_from_mesh(const ogt_mesh& gm) {
    std::vector<Rect> out;
    for (uint32_t t = 0; t + 5 < gm.index_count; t += 6) {
        IVec3 c[4] = {
            vertex_pos(gm, gm.indices[t + 0]),
            vertex_pos(gm, gm.indices[t + 1]),
            vertex_pos(gm, gm.indices[t + 2]),
            vertex_pos(gm, gm.indices[t + 4])
        };
        Rect rc;
        rc.pal = gm.vertices[gm.indices[t]].palette_index;
        rc.dir = normal_dir(gm.vertices[gm.indices[t]].normal);
        if (c[0].x == c[1].x && c[1].x == c[2].x && c[2].x == c[3].x)
            rc.axis = 0;
        else if (c[0].y == c[1].y && c[1].y == c[2].y && c[2].y == c[3].y)
            rc.axis = 1;
        else
            rc.axis = 2;
        rc.axes[0] = (rc.axis == 0) ? 1 : 0;
        rc.axes[1] = (rc.axis == 2) ? 1 : 2;
        auto coord = [&](const IVec3& p, int a) { return a == 0 ? p.x : a == 1 ? p.y : p.z; };
        rc.plane = (int)coord(c[0], rc.axis);
        for (int e = 0; e < 2; e++) {
            int a = rc.axes[e];
            int64_t mn = coord(c[0], a), mx = mn;
            for (int q = 1; q < 4; q++) {
                int64_t v = coord(c[q], a);
                if (v < mn) mn = v;
                if (v > mx) mx = v;
            }
            rc.lo[e] = (int)mn;
            rc.hi[e] = (int)mx;
        }
        out.push_back(rc);
    }
    return out;
}

// per original greedy rectangle: independent boundary points, triangle count and sub-segment edges
bool check_rectangle_structure(const ogt_mesh& gm, const ogt_mesh& m) {
    std::vector<Rect> rects = greedy_rectangles_from_mesh(gm);
    if (rects.empty())
        return m.index_count == 0;

    uint32_t checked_triangles = 0;
    for (size_t ri = 0; ri < rects.size(); ri++) {
        const Rect& rc = rects[ri];

        // triangles belonging to this rectangle: same plane and bounds within the quad, same material
        std::vector<uint32_t> tris;
        for (uint32_t t = 0; t < m.index_count; t += 3) {
            const ogt_mesh_vertex* v[3] = {
                &m.vertices[m.indices[t]], &m.vertices[m.indices[t + 1]], &m.vertices[m.indices[t + 2]]
            };
            bool match = true;
            for (int q = 0; q < 3; q++) {
                auto coordv = [&](int a) { return a == 0 ? (int)v[q]->pos.x : a == 1 ? (int)v[q]->pos.y : (int)v[q]->pos.z; };
                if (coordv(rc.axis) != rc.plane ||
                    coordv(rc.axes[0]) < rc.lo[0] || coordv(rc.axes[0]) > rc.hi[0] ||
                    coordv(rc.axes[1]) < rc.lo[1] || coordv(rc.axes[1]) > rc.hi[1])
                    match = false;
            }
            if (match) {
                if (v[0]->palette_index != rc.pal)
                    return false;
                tris.push_back(t);
            }
        }
        if (tris.empty())
            return false;
        checked_triangles += (uint32_t)tris.size();

        // independently collect boundary positions from the output mesh
        std::set<IVec3> boundary;
        for (size_t q = 0; q < tris.size(); q++) {
            uint32_t t = tris[q];
            for (int k = 0; k < 3; k++) {
                IVec3 p = vertex_pos(m, m.indices[t + k]);
                auto cp = [&](int a) { return a == 0 ? (int)p.x : a == 1 ? (int)p.y : (int)p.z; };
                int e0 = cp(rc.axes[0]), e1 = cp(rc.axes[1]);
                bool on_e0 = (e0 == rc.lo[0] || e0 == rc.hi[0]) && e1 >= rc.lo[1] && e1 <= rc.hi[1];
                bool on_e1 = (e1 == rc.lo[1] || e1 == rc.hi[1]) && e0 >= rc.lo[0] && e0 <= rc.hi[0];
                if (cp(rc.axis) == rc.plane && (on_e0 || on_e1))
                    boundary.insert(p);
            }
        }
        int n = (int)boundary.size();
        if (n < 4)
            return false;
        // boundary with n distinct points -> exactly n-2 positive-area triangles
        if ((int)tris.size() != n - 2)
            return false;
        // doubled area covers the rectangle exactly once (no overlap, no gap)
        int64_t area2 = 0;
        for (size_t q = 0; q < tris.size(); q++)
            area2 += triangle_double_area(m, tris[q]);
        int64_t expected2 = (int64_t)2 * (rc.hi[0] - rc.lo[0]) * (rc.hi[1] - rc.lo[1]);
        if (area2 != expected2)
            return false;

        // every consecutive split sub-segment on each of the four edges is a real triangle edge
        std::set<std::pair<IVec3, IVec3> > edge_set;
        for (size_t q = 0; q < tris.size(); q++) {
            uint32_t t = tris[q];
            for (int e = 0; e < 3; e++) {
                IVec3 a = vertex_pos(m, m.indices[t + e]);
                IVec3 b = vertex_pos(m, m.indices[t + (e + 1) % 3]);
                edge_set.insert(std::make_pair(a, b));
            }
        }
        for (int edge = 0; edge < 4; edge++) {
            bool along_first = (edge == 0 || edge == 2);
            int fixed_slot = (edge == 0 || edge == 3) ? 0 : 1; // which in-plane axis is held fixed
            int fixed_value = (edge < 2) ? ((edge == 0) ? rc.lo[1] : rc.hi[1])
                                         : ((edge == 2) ? rc.hi[0] : rc.lo[0]);
            (void)fixed_slot;
            std::set<int> coords;
            for (std::set<IVec3>::const_iterator it = boundary.begin(); it != boundary.end(); ++it) {
                const IVec3& p = *it;
                auto cp = [&](int a) { return a == 0 ? (int)p.x : a == 1 ? (int)p.y : (int)p.z; };
                int vary_axis = along_first ? rc.axes[0] : rc.axes[1];
                int fixed_axis = along_first ? rc.axes[1] : rc.axes[0];
                if (cp(fixed_axis) != fixed_value)
                    continue;
                coords.insert(cp(vary_axis));
            }
            std::vector<int> ordered(coords.begin(), coords.end());
            for (size_t q = 1; q < ordered.size(); q++) {
                auto make_point = [&](int vary_value) {
                    IVec3 p;
                    int va[3] = { 0, 0, 0 };
                    va[rc.axis] = rc.plane;
                    int vary_axis = along_first ? rc.axes[0] : rc.axes[1];
                    int fixed_axis = along_first ? rc.axes[1] : rc.axes[0];
                    va[vary_axis] = vary_value;
                    va[fixed_axis] = fixed_value;
                    p.x = va[0]; p.y = va[1]; p.z = va[2];
                    return p;
                };
                IVec3 a = make_point(ordered[q - 1]);
                IVec3 b = make_point(ordered[q]);
                if (!edge_set.count(std::make_pair(a, b)) && !edge_set.count(std::make_pair(b, a)))
                    return false;
            }
        }
    }
    // the rectangles partition every face, so all triangles were accounted for exactly once
    return checked_triangles == m.index_count / 3;
}

// no used vertex lies strictly inside any triangle edge, interior diagonals included
bool check_no_t_junctions(const ogt_mesh& m) {
    if (!positions_integral(m))
        return false;
    std::set<IVec3> positions;
    for (uint32_t i = 0; i < m.vertex_count; i++)
        positions.insert(vertex_pos(m, i));
    for (uint32_t t = 0; t < m.index_count; t += 3) {
        for (int e = 0; e < 3; e++) {
            IVec3 a = vertex_pos(m, m.indices[t + e]);
            IVec3 b = vertex_pos(m, m.indices[t + (e + 1) % 3]);
            int64_t ux = b.x - a.x, uy = b.y - a.y, uz = b.z - a.z;
            int64_t len2 = ux * ux + uy * uy + uz * uz;
            for (std::set<IVec3>::const_iterator it = positions.begin(); it != positions.end(); ++it) {
                const IVec3& p = *it;
                int64_t vx = p.x - a.x, vy = p.y - a.y, vz = p.z - a.z;
                int64_t dot = vx * ux + vy * uy + vz * uz;
                if (dot <= 0 || dot >= len2)
                    continue; // not strictly between the endpoints
                int64_t cx = uy * vz - uz * vy;
                int64_t cy = uz * vx - ux * vz;
                int64_t cz = ux * vy - uy * vx;
                if (cx == 0 && cy == 0 && cz == 0)
                    return false; // strict interior point on a triangle edge -> t-junction
            }
        }
    }
    return true;
}

bool check_total_area(const Grid& g, const ogt_mesh& m) {
    std::vector<Face> faces = enumerate_faces(g);
    int64_t expected2 = (int64_t)2 * (int64_t)faces.size();
    int64_t actual2 = 0;
    for (uint32_t t = 0; t < m.index_count; t += 3)
        actual2 += triangle_double_area(m, t);
    return actual2 == expected2;
}

} // namespace

// ===========================================================================
// test cases
// ===========================================================================

static const ogt_voxel_meshify_context kDefaultCtx = { NULL, NULL, NULL };

static ogt_mesh_rgba test_palette[256];

static void init_palette() {
    for (int i = 0; i < 256; i++) {
        test_palette[i].r = (uint8_t)(i * 7 + 1);
        test_palette[i].g = (uint8_t)(i * 13 + 2);
        test_palette[i].b = (uint8_t)(i * 29 + 3);
        test_palette[i].a = (uint8_t)(i ? 255 : 0);
    }
}

// ---------------------------------------------------------------------------
// coverage group
// ---------------------------------------------------------------------------

int run_cov_unit_cube() {
    init_palette();
    Grid g(1, 1, 1); g.set(0, 0, 0, 1);
    ogt_mesh* m = ogt_mesh_from_paletted_voxels_greedy_conforming(&kDefaultCtx, &g.v[0], g.nx, g.ny, g.nz, test_palette);
    TASK_CHECK(m != NULL);
    TASK_CHECK(enumerate_faces(g).size() == 6);
    TASK_CHECK(check_face_coverage(g, *m));
    TASK_CHECK(check_total_area(g, *m));
    TASK_CHECK(triangles_oriented(*m));
    TASK_CHECK(positions_integral(*m));
    TASK_CHECK(m->index_count / 3 == 12);
    ogt_mesh_destroy(&kDefaultCtx, m);
    return 0;
}

int run_cov_mixed_material_grid() {
    init_palette();
    Grid g(4, 3, 3);
    for (int z = 0; z < g.nz; z++)
        for (int y = 0; y < g.ny; y++)
            for (int x = 0; x < g.nx; x++) {
                if (x == 1 && y == 1 && z >= 1) continue; // an internal cavity/hole
                g.set(x, y, z, (uint8_t)(1 + ((x + 2 * y + 3 * z) % 3)));
            }
    ogt_mesh* m = ogt_mesh_from_paletted_voxels_greedy_conforming(&kDefaultCtx, &g.v[0], g.nx, g.ny, g.nz, test_palette);
    ogt_mesh* gm = ogt_mesh_from_paletted_voxels_greedy(&kDefaultCtx, &g.v[0], g.nx, g.ny, g.nz, test_palette);
    TASK_CHECK(m != NULL && gm != NULL);
    TASK_CHECK(check_face_coverage(g, *m));            // cover, no overlap, material
    TASK_CHECK(check_total_area(g, *m));              // exact area vs independent face enumeration
    TASK_CHECK(triangles_oriented(*m));               // orientation and uniform face attributes
    TASK_CHECK(positions_integral(*m));
    TASK_CHECK(check_rectangle_structure(*gm, *m));   // per-original-rectangle boundary/triangle checks
    TASK_CHECK(m->index_count / 3 >= gm->index_count / 3); // conforming never has fewer triangles
    ogt_mesh_destroy(&kDefaultCtx, m);
    ogt_mesh_destroy(&kDefaultCtx, gm);
    return 0;
}

int run_cov_equals_simple_area_on_random_layout() {
    init_palette();
    Grid g(6, 5, 4);
    // deterministic irregular layout with several materials
    for (int z = 0; z < g.nz; z++)
        for (int y = 0; y < g.ny; y++)
            for (int x = 0; x < g.nx; x++) {
                int h = (x * 5 + y * 11 + z * 7) % 11;
                if (h < 4) g.set(x, y, z, (uint8_t)(1 + h % 3));
            }
    ogt_mesh* m = ogt_mesh_from_paletted_voxels_greedy_conforming(&kDefaultCtx, &g.v[0], g.nx, g.ny, g.nz, test_palette);
    ogt_mesh* sm = ogt_mesh_from_paletted_voxels_simple(&kDefaultCtx, &g.v[0], g.nx, g.ny, g.nz, test_palette);
    TASK_CHECK(m != NULL && sm != NULL);
    TASK_CHECK(check_face_coverage(g, *m));
    // the simple mesher is an independent area reference: total doubled area must match
    int64_t area_simple = 0, area_conf = 0;
    for (uint32_t t = 0; t < sm->index_count; t += 3) area_simple += triangle_double_area(*sm, t);
    for (uint32_t t = 0; t < m->index_count; t += 3) area_conf += triangle_double_area(*m, t);
    TASK_CHECK(area_conf == area_simple);
    TASK_CHECK(triangles_oriented(*m));
    ogt_mesh_destroy(&kDefaultCtx, m);
    ogt_mesh_destroy(&kDefaultCtx, sm);
    return 0;
}

// ---------------------------------------------------------------------------
// seams group (all relations checked on integer coordinates)
// ---------------------------------------------------------------------------

int run_seam_staircase() {
    init_palette();
    // three cubes in an L/stair so several large greedy faces meet smaller ones
    Grid g(3, 3, 1);
    g.set(0, 0, 0, 1); g.set(1, 0, 0, 1); g.set(2, 0, 0, 1);
    g.set(2, 1, 0, 1); g.set(2, 2, 0, 1);
    ogt_mesh* m = ogt_mesh_from_paletted_voxels_greedy_conforming(&kDefaultCtx, &g.v[0], g.nx, g.ny, g.nz, test_palette);
    TASK_CHECK(m != NULL);
    TASK_CHECK(check_no_t_junctions(*m));
    TASK_CHECK(check_total_area(g, *m));
    ogt_mesh_destroy(&kDefaultCtx, m);
    return 0;
}

int run_seam_same_and_different_color() {
    init_palette();
    // indices 1 and 2 carry identical RGBA: the conforming path must still treat them as different materials,
    // because grouping is by palette_index, never by color value.
    test_palette[2] = test_palette[1];
    Grid g(4, 2, 1);
    // same-color run then a different material: both junction kinds must be split
    g.set(0, 0, 0, 1); g.set(1, 0, 0, 1); g.set(2, 0, 0, 1); g.set(3, 0, 0, 2);
    g.set(0, 1, 0, 1); g.set(1, 1, 0, 2); g.set(2, 1, 0, 2); g.set(3, 1, 0, 2);
    ogt_mesh* m = ogt_mesh_from_paletted_voxels_greedy_conforming(&kDefaultCtx, &g.v[0], g.nx, g.ny, g.nz, test_palette);
    ogt_mesh* gm = ogt_mesh_from_paletted_voxels_greedy(&kDefaultCtx, &g.v[0], g.nx, g.ny, g.nz, test_palette);
    TASK_CHECK(m != NULL && gm != NULL);
    TASK_CHECK(check_no_t_junctions(*m));
    TASK_CHECK(check_total_area(g, *m));
    // both materials survive as distinct face attributes even though their RGBA is identical
    bool saw1 = false, saw2 = false;
    for (uint32_t i = 0; i < m->vertex_count; i++) {
        if (m->vertices[i].palette_index == 1) saw1 = true;
        if (m->vertices[i].palette_index == 2) saw2 = true;
    }
    TASK_CHECK(saw1 && saw2);
    // identical RGBA must not cause merging: the greedy partition keeps the two indices apart
    int gm1 = 0, gm2 = 0;
    for (uint32_t i = 0; i < gm->vertex_count; i++) {
        if (gm->vertices[i].palette_index == 1) gm1++;
        if (gm->vertices[i].palette_index == 2) gm2++;
    }
    TASK_CHECK(gm1 > 0 && gm2 > 0);
    ogt_mesh_destroy(&kDefaultCtx, m);
    ogt_mesh_destroy(&kDefaultCtx, gm);
    return 0;
}

int run_seam_perpendicular_faces() {
    init_palette();
    // a tall slab next to a short one: a horizontal face corner lands strictly inside a vertical edge
    Grid g(2, 2, 3);
    g.set(0, 0, 0, 1); g.set(1, 0, 0, 1);
    g.set(0, 0, 1, 1); g.set(1, 0, 1, 1);
    g.set(0, 0, 2, 1); // extra height only on the x=0 column -> split across the perpendicular side face
    g.set(0, 1, 0, 1); g.set(1, 1, 0, 1);
    g.set(0, 1, 1, 1); g.set(1, 1, 1, 1);
    g.set(0, 1, 2, 1);
    ogt_mesh* m = ogt_mesh_from_paletted_voxels_greedy_conforming(&kDefaultCtx, &g.v[0], g.nx, g.ny, g.nz, test_palette);
    TASK_CHECK(m != NULL);
    TASK_CHECK(check_no_t_junctions(*m));
    TASK_CHECK(check_total_area(g, *m));
    ogt_mesh_destroy(&kDefaultCtx, m);
    return 0;
}

int run_seam_ring_with_hole() {
    init_palette();
    Grid g(5, 5, 1);
    for (int y = 0; y < 5; y++)
        for (int x = 0; x < 5; x++)
            if (!(x >= 1 && x <= 3 && y >= 1 && y <= 3))
                g.set(x, y, 0, 1); // hollow square ring
    ogt_mesh* m = ogt_mesh_from_paletted_voxels_greedy_conforming(&kDefaultCtx, &g.v[0], g.nx, g.ny, g.nz, test_palette);
    TASK_CHECK(m != NULL);
    TASK_CHECK(check_no_t_junctions(*m));
    TASK_CHECK(check_total_area(g, *m));
    TASK_CHECK(check_face_coverage(g, *m));
    ogt_mesh_destroy(&kDefaultCtx, m);
    return 0;
}

int run_seam_edge_and_point_contact() {
    init_palette();
    // two solid blocks that touch only along a single vertical edge
    Grid g(2, 2, 2);
    g.set(0, 0, 0, 1); g.set(0, 0, 1, 1);
    g.set(1, 1, 0, 2); g.set(1, 1, 1, 2);
    ogt_mesh* m = ogt_mesh_from_paletted_voxels_greedy_conforming(&kDefaultCtx, &g.v[0], g.nx, g.ny, g.nz, test_palette);
    TASK_CHECK(m != NULL);
    TASK_CHECK(check_no_t_junctions(*m));
    TASK_CHECK(check_total_area(g, *m));
    // non-manifold contact is retained: the shared geometric edge is referenced by more than two triangles
    std::map<std::pair<IVec3, IVec3>, int> edge_use;
    for (uint32_t t = 0; t < m->index_count; t += 3)
        for (int e = 0; e < 3; e++) {
            IVec3 a = vertex_pos(*m, m->indices[t + e]);
            IVec3 b = vertex_pos(*m, m->indices[t + (e + 1) % 3]);
            if (b < a) { IVec3 tmp = a; a = b; b = tmp; }
            edge_use[std::make_pair(a, b)]++;
        }
    int high_multiplicity = 0;
    for (std::map<std::pair<IVec3, IVec3>, int>::const_iterator it = edge_use.begin(); it != edge_use.end(); ++it)
        if (it->second > 2) high_multiplicity++;
    TASK_CHECK(high_multiplicity > 0);
    ogt_mesh_destroy(&kDefaultCtx, m);
    return 0;
}

// ---------------------------------------------------------------------------
// ownership group
// ---------------------------------------------------------------------------

struct AllocTracker {
    int live;
    int calls;
    int fail_after; // when >=0, the (fail_after+1)th allocation returns NULL
};

static void* tracking_alloc(size_t n, void* user) {
    AllocTracker* t = (AllocTracker*)user;
    t->calls++;
    if (t->fail_after >= 0 && t->calls > t->fail_after)
        return NULL;
    void* p = malloc(n);
    if (p) t->live++;
    return p;
}
static void tracking_free(void* p, void* user) {
    if (!p) return;
    AllocTracker* t = (AllocTracker*)user;
    t->live--;
    free(p);
}

int run_own_allocator_pairing_and_failures() {
    init_palette();
    Grid g(7, 6, 5);
    for (int z = 0; z < g.nz; z++)
        for (int y = 0; y < g.ny; y++)
            for (int x = 0; x < g.nx; x++)
                if ((x + y * 3 + z * 5) % 4) g.set(x, y, z, (uint8_t)(1 + (x + y + z) % 2));

    // success path: every allocation is eventually released
    AllocTracker ok = { 0, 0, -1 };
    ogt_voxel_meshify_context ctx = { tracking_alloc, tracking_free, &ok };
    ogt_mesh* m = ogt_mesh_from_paletted_voxels_greedy_conforming(&ctx, &g.v[0], g.nx, g.ny, g.nz, test_palette);
    TASK_CHECK(m != NULL);
    ogt_mesh_destroy(&ctx, m);
    TASK_CHECK(ok.live == 0);

    // every possible failure point must clean up fully and leave the tracker balanced
    for (int fail = 1; fail <= ok.calls + 2; fail++) {
        AllocTracker f = { 0, 0, fail };
        ogt_voxel_meshify_context fctx = { tracking_alloc, tracking_free, &f };
        ogt_mesh* fm = ogt_mesh_from_paletted_voxels_greedy_conforming(&fctx, &g.v[0], g.nx, g.ny, g.nz, test_palette);
        if (fm) ogt_mesh_destroy(&fctx, fm);
        TASK_CHECK(f.live == 0);
    }
    return 0;
}

int run_own_repeat_and_input_unchanged() {
    init_palette();
    Grid g(5, 4, 3);
    for (int z = 0; z < g.nz; z++)
        for (int y = 0; y < g.ny; y++)
            for (int x = 0; x < g.nx; x++)
                g.set(x, y, z, (uint8_t)(1 + (x * 2 + y + z) % 3));
    std::vector<uint8_t> backup = g.v;

    ogt_mesh* m1 = ogt_mesh_from_paletted_voxels_greedy_conforming(&kDefaultCtx, &g.v[0], g.nx, g.ny, g.nz, test_palette);
    ogt_mesh* m2 = ogt_mesh_from_paletted_voxels_greedy_conforming(&kDefaultCtx, &g.v[0], g.nx, g.ny, g.nz, test_palette);
    TASK_CHECK(m1 != NULL && m2 != NULL);
    // input is read-only
    TASK_CHECK(memcmp(&g.v[0], &backup[0], g.v.size()) == 0);
    // repeatable: identical vertex fields and index sequence (no padding bytes compared)
    TASK_CHECK(m1->vertex_count == m2->vertex_count);
    TASK_CHECK(m1->index_count == m2->index_count);
    TASK_CHECK(memcmp(m1->indices, m2->indices, m1->index_count * sizeof(uint32_t)) == 0);
    bool same_vertices = true;
    for (uint32_t i = 0; i < m1->vertex_count; i++) {
        const ogt_mesh_vertex& a = m1->vertices[i];
        const ogt_mesh_vertex& b = m2->vertices[i];
        if (memcmp(&a.pos, &b.pos, sizeof(ogt_mesh_vec3)) != 0 ||
            memcmp(&a.normal, &b.normal, sizeof(ogt_mesh_vec3)) != 0 ||
            memcmp(&a.color, &b.color, sizeof(ogt_mesh_rgba)) != 0 ||
            a.palette_index != b.palette_index)
            same_vertices = false;
    }
    TASK_CHECK(same_vertices);
    ogt_mesh_destroy(&kDefaultCtx, m1);
    ogt_mesh_destroy(&kDefaultCtx, m2);
    return 0;
}

int run_own_solid_block_and_edge_cases() {
    init_palette();
    // 24 x 17 x 9 fully solid: six big rectangles, two triangles each -> 12 triangles
    Grid block(24, 17, 9);
    for (size_t i = 0; i < block.v.size(); i++) block.v[i] = 1;
    ogt_mesh* m = ogt_mesh_from_paletted_voxels_greedy_conforming(&kDefaultCtx, &block.v[0], block.nx, block.ny, block.nz, test_palette);
    TASK_CHECK(m != NULL);
    TASK_CHECK(m->index_count / 3 == 12);
    TASK_CHECK(check_no_t_junctions(*m));
    ogt_mesh_destroy(&kDefaultCtx, m);

    // maximum supported dimension (64) is accepted and a solid run still yields a clean two-triangle face set
    Grid max_line(64, 1, 1);
    for (size_t i = 0; i < max_line.v.size(); i++) max_line.v[i] = 1;
    ogt_mesh* xm = ogt_mesh_from_paletted_voxels_greedy_conforming(&kDefaultCtx, &max_line.v[0], 64, 1, 1, test_palette);
    TASK_CHECK(xm != NULL);
    TASK_CHECK(check_no_t_junctions(*xm));
    TASK_CHECK(check_total_area(max_line, *xm));
    ogt_mesh_destroy(&kDefaultCtx, xm);

    // empty voxel set returns a valid, destroyable empty mesh
    Grid emptyv(3, 3, 3);
    ogt_mesh* e = ogt_mesh_from_paletted_voxels_greedy_conforming(&kDefaultCtx, &emptyv.v[0], 3, 3, 3, test_palette);
    TASK_CHECK(e != NULL);
    TASK_CHECK(e->index_count == 0 && e->vertex_count == 0);
    ogt_mesh_destroy(&kDefaultCtx, e);

    // invalid arguments
    TASK_CHECK(ogt_mesh_from_paletted_voxels_greedy_conforming(&kDefaultCtx, NULL, 1, 1, 1, test_palette) == NULL);
    TASK_CHECK(ogt_mesh_from_paletted_voxels_greedy_conforming(&kDefaultCtx, &block.v[0], 1, 1, 1, NULL) == NULL);
    TASK_CHECK(ogt_mesh_from_paletted_voxels_greedy_conforming(&kDefaultCtx, &block.v[0], 65, 1, 1, test_palette) == NULL);
    TASK_CHECK(ogt_mesh_from_paletted_voxels_greedy_conforming(&kDefaultCtx, &block.v[0], 1, 0, 1, test_palette) == NULL);
    TASK_CHECK(ogt_mesh_from_paletted_voxels_greedy_conforming(NULL, &block.v[0], 1, 1, 1, test_palette) == NULL);

    // occupancy ignores alpha: an index with a fully transparent RGBA is still solid
    ogt_mesh_rgba transparent_palette[256];
    memset(transparent_palette, 0, sizeof(transparent_palette));
    transparent_palette[1].a = 0;
    Grid alpha_grid(1, 1, 1); alpha_grid.set(0, 0, 0, 1);
    ogt_mesh* am = ogt_mesh_from_paletted_voxels_greedy_conforming(&kDefaultCtx, &alpha_grid.v[0], 1, 1, 1, transparent_palette);
    TASK_CHECK(am != NULL);
    TASK_CHECK(am->index_count == 36); // a single solid voxel always has six faces, regardless of alpha
    ogt_mesh_destroy(&kDefaultCtx, am);
    return 0;
}

// ---------------------------------------------------------------------------
// registration
// ---------------------------------------------------------------------------

const struct task_case task_cases[] = {
    { "coverage", "unit_cube",              run_cov_unit_cube },
    { "coverage", "mixed_material_grid",    run_cov_mixed_material_grid },
    { "coverage", "simple_area_reference",  run_cov_equals_simple_area_on_random_layout },

    { "seams", "staircase",                 run_seam_staircase },
    { "seams", "same_and_different_color",  run_seam_same_and_different_color },
    { "seams", "perpendicular_faces",       run_seam_perpendicular_faces },
    { "seams", "ring_with_hole",            run_seam_ring_with_hole },
    { "seams", "edge_point_contact",        run_seam_edge_and_point_contact },

    { "ownership", "allocator_pairing",     run_own_allocator_pairing_and_failures },
    { "ownership", "repeat_and_input",      run_own_repeat_and_input_unchanged },
    { "ownership", "solid_block_edge_cases",run_own_solid_block_and_edge_cases },
};
const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
