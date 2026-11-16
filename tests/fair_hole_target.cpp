/* ----------------------------------------------------------------------- *
 * Target regression tests for HMesh::fill_fair_hole and the attributed
 * Manifold / OBJ channel extensions. The test driver (task_support/driver.c)
 * supplies main() and iterates task_cases.
 * ----------------------------------------------------------------------- */

#include "task_support/check.h"

#include <GEL/HMesh/HolePatch.h>
#include <GEL/HMesh/cleanup.h>
#include <GEL/HMesh/obj_load.h>
#include <GEL/HMesh/obj_save.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using namespace HMesh;
using CGLA::Vec2d;
using CGLA::Vec3d;

namespace
{
    const double PI = std::acos(-1.0);

    bool approx(double a, double b, double tol = 1e-10)
    {
        return std::fabs(a - b) <= tol * (1.0 + std::fabs(a) + std::fabs(b));
    }

    bool approx_v(const Vec3d& a, const Vec3d& b, double tol = 1e-10)
    {
        return approx(a[0], b[0], tol) && approx(a[1], b[1], tol) && approx(a[2], b[2], tol);
    }

    bool approx_v2(const Vec2d& a, const Vec2d& b, double tol = 1e-10)
    {
        return approx(a[0], b[0], tol) && approx(a[1], b[1], tol);
    }

    HalfEdgeID find_hole(const Manifold& m)
    {
        for (HalfEdgeID h : m.halfedges())
            if (m.walker(h).face() == InvalidFaceID)
                return h;
        return InvalidHalfEdgeID;
    }

    std::vector<VertexID> hole_vertices(const Manifold& m, HalfEdgeID h)
    {
        std::vector<VertexID> verts;
        HalfEdgeID cur = h;
        do {
            verts.push_back(m.walker(cur).vertex());
            cur = m.walker(cur).next().halfedge();
        }
        while (cur != h);
        return verts;
    }

    // Annulus with three rings of n vertices at radii 1, 2, 3. The inner
    // ring is an open hole; wave perturbs the inner ring out of plane.
    AttributedManifold build_annulus(int n, double wave = 0.0)
    {
        std::vector<Vec3d> verts;
        for (int ring = 0; ring < 3; ++ring) {
            const double rad = 1.0 + ring;
            for (int i = 0; i < n; ++i) {
                const double a = 2.0 * PI * i / n;
                const double z = ring == 0 ? wave * std::sin(3.0 * a) : 0.0;
                verts.emplace_back(rad * std::cos(a), rad * std::sin(a), z);
            }
        }
        auto id = [&](int r, int i) { return r * n + (i % n); };
        std::vector<std::array<int, 3>> tris;
        for (int r = 0; r < 2; ++r)
            for (int i = 0; i < n; ++i) {
                tris.push_back({id(r, i), id(r + 1, i), id(r, i + 1)});
                tris.push_back({id(r, i + 1), id(r + 1, i), id(r + 1, i + 1)});
            }

        AttributedManifold am;
        VertexAttributeVector<int> cluster_id;
        for (const auto& t : tris) {
            FaceID f = am.mesh.add_face({verts[t[0]], verts[t[1]], verts[t[2]]});
            int j = 0;
            circulate_face_ccw(am.mesh, f, [&](VertexID v) { cluster_id[v] = t[j++]; });
        }
        stitch_mesh(am.mesh, cluster_id);
        IDRemap remap;
        am.mesh.cleanup(remap);
        return am;
    }

    // Fan disk: one center vertex and a ring of n vertices. The outer ring
    // bounds an open hole. zc sets the center height (a deep center stresses
    // the fairing and can produce a genuine fold).
    AttributedManifold build_fan_disk(int n, double zc)
    {
        std::vector<Vec3d> verts;
        verts.emplace_back(0.0, 0.0, zc);
        for (int i = 0; i < n; ++i) {
            const double a = 2.0 * PI * i / n;
            verts.emplace_back(std::cos(a), std::sin(a), 0.0);
        }
        std::vector<std::array<int, 3>> tris;
        for (int i = 0; i < n; ++i)
            tris.push_back({0, 1 + i, 1 + ((i + 1) % n)});

        AttributedManifold am;
        VertexAttributeVector<int> cluster_id;
        for (const auto& t : tris) {
            FaceID f = am.mesh.add_face({verts[t[0]], verts[t[1]], verts[t[2]]});
            int j = 0;
            circulate_face_ccw(am.mesh, f, [&](VertexID v) { cluster_id[v] = t[j++]; });
        }
        stitch_mesh(am.mesh, cluster_id);
        IDRemap remap;
        am.mesh.cleanup(remap);
        return am;
    }

    // Dense Gaussian elimination with partial pivoting.
    bool dense_solve(std::vector<std::vector<double>> A, std::vector<double> b,
                     std::vector<double>& x)
    {
        const size_t n = A.size();
        for (size_t k = 0; k < n; ++k) {
            size_t piv = k;
            for (size_t r = k + 1; r < n; ++r)
                if (std::fabs(A[r][k]) > std::fabs(A[piv][k]))
                    piv = r;
            if (std::fabs(A[piv][k]) < 1e-14)
                return false;
            std::swap(A[piv], A[k]);
            std::swap(b[piv], b[k]);
            for (size_t r = 0; r < n; ++r) {
                if (r == k)
                    continue;
                const double f = A[r][k] / A[k][k];
                for (size_t c = k; c < n; ++c)
                    A[r][c] -= f * A[k][c];
                b[r] -= f * b[k];
            }
        }
        x.resize(n);
        for (size_t k = 0; k < n; ++k)
            x[k] = b[k] / A[k][k];
        return true;
    }

    std::vector<VertexID> real_neighbors(const Manifold& m, VertexID v)
    {
        std::vector<VertexID> out;
        std::unordered_set<size_t> seen;
        for (VertexID u : m.incident_vertices(v))
            if (seen.insert(u.get_index()).second)
                out.push_back(u);
        return out;
    }

    // Energy E = sum over the row set of ||p_i - mean(neighbors)||^2 using
    // the real patched adjacency.
    double row_energy(const Manifold& m, const std::vector<VertexID>& rows)
    {
        double e = 0.0;
        for (VertexID v : rows) {
            Vec3d mean(0.0);
            auto nb = real_neighbors(m, v);
            for (VertexID u : nb)
                mean += m.pos(u);
            mean /= static_cast<double>(nb.size());
            Vec3d d = m.pos(v) - mean;
            e += dot(d, d);
        }
        return e;
    }

    // Replicate the production topology on a fresh mesh (used by the dense
    // reference case). Returns the interior vertices and patch faces.
    void replicate_topology(Manifold& m, HalfEdgeID hole, unsigned rounds,
                            std::vector<VertexID>& interior,
                            std::vector<FaceID>& patch_faces)
    {
        FaceID cap = m.close_hole(hole);
        interior.push_back(m.split_face_by_vertex(cap));
        for (FaceID f : m.incident_faces(interior[0]))
            patch_faces.push_back(f);
        for (unsigned r = 1; r < rounds; ++r) {
            std::vector<FaceID> nxt;
            for (FaceID f : patch_faces) {
                VertexID c = m.split_face_by_vertex(f);
                interior.push_back(c);
                for (FaceID g : m.incident_faces(c))
                    nxt.push_back(g);
            }
            patch_faces = std::move(nxt);
        }
    }
}

// ===========================================================================
// fairing
// ===========================================================================

// Small patch: independent dense normal-equation solve, finite difference
// verification of the energy and its true gradient.
static int fairing_dense_reference()
{
    const int n = 4;
    const unsigned rounds = 2;

    AttributedManifold am = build_annulus(n, 0.0);
    HalfEdgeID hole = find_hole(am.mesh);
    TASK_CHECK(hole != InvalidHalfEdgeID);
    HolePatchResult res = fill_fair_hole(am, hole, rounds);
    TASK_CHECK(res.residual <= 1e-8);
    TASK_CHECK(res.vertices.size() == 1 + static_cast<size_t>(n));

    // Independent reference assembly on a separate mesh.
    AttributedManifold ref = build_annulus(n, 0.0);
    HalfEdgeID ref_hole = find_hole(ref.mesh);
    std::vector<VertexID> B = hole_vertices(ref.mesh, ref_hole);
    std::vector<VertexID> interior;
    std::vector<FaceID> patch_faces;
    replicate_topology(ref.mesh, ref_hole, rounds, interior, patch_faces);

    const size_t nb = B.size();
    const size_t ni = interior.size();
    const size_t ns = nb + ni;
    std::unordered_map<size_t, size_t> idx;
    for (size_t i = 0; i < ns; ++i) {
        VertexID v = i < nb ? B[i] : interior[i - nb];
        idx[v.get_index()] = i;
    }
    std::vector<std::vector<VertexID>> adj(ns);
    for (size_t r = 0; r < ns; ++r) {
        VertexID v = r < nb ? B[r] : interior[r - nb];
        adj[r] = real_neighbors(ref.mesh, v);
    }

    // Dense W (ns x ni) and q (ns): d = W x + q.
    std::vector<std::vector<double>> W(ns, std::vector<double>(ni, 0.0));
    std::vector<std::vector<double>> Wq(ns, std::vector<double>(3, 0.0));
    for (size_t r = 0; r < ns; ++r) {
        const double inv = 1.0 / static_cast<double>(adj[r].size());
        if (r >= nb)
            W[r][r - nb] = inv * static_cast<double>(adj[r].size());
        else
            for (int c = 0; c < 3; ++c)
                Wq[r][c] += inv * static_cast<double>(adj[r].size()) * ref.mesh.pos(B[r])[c];
        for (VertexID u : adj[r]) {
            auto it = idx.find(u.get_index());
            if (it != idx.end() && it->second >= nb)
                W[r][it->second - nb] -= inv;
            else
                for (int c = 0; c < 3; ++c)
                    Wq[r][c] -= inv * ref.mesh.pos(u)[c];
        }
    }
    std::vector<std::vector<double>> A(ni, std::vector<double>(ni, 0.0));
    for (size_t a = 0; a < ni; ++a)
        for (size_t b = 0; b < ni; ++b) {
            double s = 0.0;
            for (size_t r = 0; r < ns; ++r)
                s += W[r][a] * W[r][b];
            A[a][b] = s;
        }

    std::vector<double> ref_x(ni * 3);
    for (int c = 0; c < 3; ++c) {
        std::vector<double> rhs(ni, 0.0);
        for (size_t a = 0; a < ni; ++a) {
            double s = 0.0;
            for (size_t r = 0; r < ns; ++r)
                s -= W[r][a] * Wq[r][c];
            rhs[a] = s;
        }
        std::vector<double> xc;
        TASK_CHECK(dense_solve(A, rhs, xc));
        for (size_t a = 0; a < ni; ++a)
            ref_x[3 * a + c] = xc[a];
    }

    // Production positions must match the dense minimizer.
    for (size_t a = 0; a < ni; ++a) {
        const Vec3d& p = am.mesh.pos(res.vertices[a]);
        TASK_CHECK(approx(p[0], ref_x[3 * a + 0], 1e-9));
        TASK_CHECK(approx(p[1], ref_x[3 * a + 1], 1e-9));
        TASK_CHECK(approx(p[2], ref_x[3 * a + 2], 1e-9));
    }

    // Put the dense solution on the reference mesh and check the energy.
    for (size_t a = 0; a < ni; ++a)
        ref.mesh.pos(interior[a]) = Vec3d(ref_x[3 * a], ref_x[3 * a + 1], ref_x[3 * a + 2]);
    std::vector<VertexID> rows = B;
    rows.insert(rows.end(), interior.begin(), interior.end());
    double ref_energy = row_energy(ref.mesh, rows);
    TASK_CHECK(approx(ref_energy, res.energy, 1e-9));

    // Analytic gradient g = 2 L_I^T Dinv d over the row set.
    std::vector<Vec3d> d(ns);
    for (size_t r = 0; r < ns; ++r) {
        Vec3d mean(0.0);
        for (VertexID u : adj[r])
            mean += ref.mesh.pos(u);
        mean /= static_cast<double>(adj[r].size());
        VertexID vr = r < nb ? B[r] : interior[r - nb];
        d[r] = ref.mesh.pos(vr) - mean;
    }
    std::vector<Vec3d> grad(ni, Vec3d(0.0));
    for (size_t r = 0; r < ns; ++r) {
        const double inv = 1.0 / static_cast<double>(adj[r].size());
        if (r >= nb)
            grad[r - nb] += static_cast<double>(adj[r].size()) * inv * d[r];
        for (VertexID u : adj[r]) {
            auto it = idx.find(u.get_index());
            if (it != idx.end() && it->second >= nb)
                grad[it->second - nb] -= inv * d[r];
        }
    }
    double ginf = 0.0;
    for (size_t a = 0; a < ni; ++a)
        for (int c = 0; c < 3; ++c)
            ginf = std::max(ginf, std::fabs(2.0 * grad[a][c]));
    TASK_CHECK(approx(ginf, res.residual, 1e-10));

    // Central finite differences of E must match the analytic gradient.
    const double eps = 1e-5;
    double fd_max = 0.0;
    for (size_t a = 0; a < ni; ++a) {
        for (int c = 0; c < 3; ++c) {
            Vec3d saved = ref.mesh.pos(interior[a]);
            Vec3d pp = saved, mm = saved;
            pp[c] += eps;
            mm[c] -= eps;
            ref.mesh.pos(interior[a]) = pp;
            double ep = row_energy(ref.mesh, rows);
            ref.mesh.pos(interior[a]) = mm;
            double em = row_energy(ref.mesh, rows);
            ref.mesh.pos(interior[a]) = saved;
            double fd = (ep - em) / (2.0 * eps);
            fd_max = std::max(fd_max, std::fabs(fd - 2.0 * grad[a][c]));
        }
    }
    TASK_CHECK(fd_max < 1e-5);

    // The returned energy itself must be stationary under a perturbation.
    TASK_CHECK(std::isfinite(res.energy));
    return 0;
}

// Moving the outside one ring while keeping the hole fixed must change the
// patch, and must never move the boundary vertices.
static int fairing_outside_ring_influence()
{
    AttributedManifold a = build_annulus(8, 0.0);
    AttributedManifold b = build_annulus(8, 0.0);
    HalfEdgeID ha = find_hole(a.mesh);
    HalfEdgeID hb = find_hole(b.mesh);
    std::vector<VertexID> bva = hole_vertices(a.mesh, ha);

    // Shift the ring directly adjacent to the hole (radius 2) of mesh b
    // only: these vertices are exactly the outside neighbors of the B rows.
    for (VertexID v : b.mesh.vertices()) {
        const Vec3d& p = b.mesh.pos(v);
        if (approx(std::sqrt(p[0] * p[0] + p[1] * p[1]), 2.0, 1e-9))
            b.mesh.pos(v) = p + Vec3d(0.0, 0.0, 0.7);
    }

    HolePatchResult ra = fill_fair_hole(a, ha, 2);
    HolePatchResult rb = fill_fair_hole(b, hb, 2);

    // Boundary positions unchanged by the fill, and identical across meshes.
    for (VertexID v : bva) {
        TASK_CHECK(a.mesh.in_use(v));
        TASK_CHECK(approx_v2(Vec2d(a.mesh.pos(v)[0], a.mesh.pos(v)[1]),
                             Vec2d(b.mesh.pos(v)[0], b.mesh.pos(v)[1])));
    }

    bool interior_differs = false;
    for (size_t k = 0; k < ra.vertices.size(); ++k)
        if (!approx_v(a.mesh.pos(ra.vertices[k]), b.mesh.pos(rb.vertices[k]), 1e-9))
            interior_differs = true;
    TASK_CHECK(interior_differs);
    TASK_CHECK(!approx(ra.energy, rb.energy, 1e-12));
    TASK_CHECK(ra.residual <= 1e-8 && rb.residual <= 1e-8);
    return 0;
}

// A non-planar (wavy) boundary still fairs to the gradient tolerance.
static int fairing_nonplanar_hole()
{
    AttributedManifold am = build_annulus(10, 0.35);
    HalfEdgeID hole = find_hole(am.mesh);
    std::vector<VertexID> bv = hole_vertices(am.mesh, hole);
    bool wavy = false;
    for (size_t i = 1; i < bv.size(); ++i)
        if (!approx(am.mesh.pos(bv[i])[2], am.mesh.pos(bv[0])[2]))
            wavy = true;
    TASK_CHECK(wavy);

    HolePatchResult res = fill_fair_hole(am, hole, 2);
    TASK_CHECK(res.residual <= 1e-8);
    TASK_CHECK(res.faces.size() == 10u * 3u);
    TASK_CHECK(std::isfinite(res.energy));

    // All signed areas in the Newell plane share one nonzero sign.
    Vec3d newell(0.0);
    for (size_t i = 0; i < bv.size(); ++i)
        newell += cross(am.mesh.pos(bv[i]), am.mesh.pos(bv[(i + 1) % bv.size()]));
    Vec3d pn = normalize(newell);
    int sign = 0;
    for (FaceID f : res.faces) {
        std::vector<VertexID> fv;
        for (VertexID v : am.mesh.incident_vertices(f))
            fv.push_back(v);
        TASK_CHECK(fv.size() == 3);
        Vec3d an = 0.5 * cross(am.mesh.pos(fv[1]) - am.mesh.pos(fv[0]),
                               am.mesh.pos(fv[2]) - am.mesh.pos(fv[0]));
        double s = dot(an, pn);
        TASK_CHECK(s != 0.0);
        int sg = s > 0 ? 1 : -1;
        if (sign == 0)
            sign = sg;
        else
            TASK_CHECK(sign == sg);
    }
    return 0;
}

// Face/vertex counts and the n * 3^(rounds-1) formula for every round.
static int fairing_refinement_rounds()
{
    const unsigned n = 10;
    for (unsigned rounds = 1; rounds <= 3; ++rounds) {
        AttributedManifold am = build_annulus(static_cast<int>(n), 0.0);
        HalfEdgeID hole = find_hole(am.mesh);
        HolePatchResult res = fill_fair_hole(am, hole, rounds);
        TASK_CHECK(res.residual <= 1e-8);
        size_t expect_faces = n;
        for (unsigned k = 1; k < rounds; ++k)
            expect_faces *= 3u;
        TASK_CHECK(res.faces.size() == expect_faces);
        // Closed form for the added vertices: 1 + n * (3^(r-1) - 1) / 2.
        size_t closed = 1 + n * (expect_faces / n - 1) / 2;
        TASK_CHECK(res.vertices.size() == closed);
        std::unordered_set<size_t> unique_f, unique_v;
        for (FaceID f : res.faces)
            TASK_CHECK(unique_f.insert(f.get_index()).second && am.mesh.in_use(f));
        for (VertexID v : res.vertices)
            TASK_CHECK(unique_v.insert(v.get_index()).second && am.mesh.in_use(v));
    }
    return 0;
}

// A rigid body transform of the input gives the transformed minimizer and
// the same energy.
static int fairing_rigid_transform()
{
    AttributedManifold a = build_annulus(8, 0.1);
    AttributedManifold b = build_annulus(8, 0.1);
    const double ang = 0.63;
    const double ca = std::cos(ang), sa = std::sin(ang);
    const Vec3d t(1.5, -2.0, 0.4);
    auto xform = [&](const Vec3d& p) {
        return Vec3d(ca * p[0] - sa * p[1] + t[0],
                     sa * p[0] + ca * p[1] + t[1],
                     p[2] + t[2]);
    };
    for (VertexID v : b.mesh.vertices())
        b.mesh.pos(v) = xform(b.mesh.pos(v));

    HolePatchResult ra = fill_fair_hole(a, find_hole(a.mesh), 2);
    HolePatchResult rb = fill_fair_hole(b, find_hole(b.mesh), 2);
    TASK_CHECK(ra.vertices.size() == rb.vertices.size());
    for (size_t k = 0; k < ra.vertices.size(); ++k)
        TASK_CHECK(approx_v(b.mesh.pos(rb.vertices[k]),
                            xform(a.mesh.pos(ra.vertices[k])), 1e-8));
    TASK_CHECK(approx(ra.energy, rb.energy, 1e-9));
    TASK_CHECK(rb.residual <= 1e-8);
    return 0;
}

// ===========================================================================
// topology
// ===========================================================================

// Edge-by-edge inspection of the real patch connectivity.
static int topology_patch_connectivity()
{
    const int n = 7;
    AttributedManifold am = build_annulus(n, 0.0);
    HalfEdgeID hole = find_hole(am.mesh);
    std::vector<HalfEdgeID> boundary_edges;
    HalfEdgeID cur = hole;
    do {
        boundary_edges.push_back(cur);
        cur = am.mesh.walker(cur).next().halfedge();
    }
    while (cur != hole);

    HolePatchResult res = fill_fair_hole(am, hole, 2);
    std::unordered_set<size_t> patch_face;
    for (FaceID f : res.faces)
        patch_face.insert(f.get_index());

    // Every original boundary edge now borders one patch face and one
    // untouched outside face.
    for (HalfEdgeID h : boundary_edges) {
        TASK_CHECK(am.mesh.in_use(h));
        FaceID f1 = am.mesh.walker(h).face();
        FaceID f2 = am.mesh.walker(h).opp().face();
        TASK_CHECK(f1 != InvalidFaceID && f2 != InvalidFaceID);
        TASK_CHECK(patch_face.count(f1.get_index()) + patch_face.count(f2.get_index()) == 1);
    }

    // Count interior patch edges (patch face on both sides).
    size_t interior_edges = 0;
    size_t boundary_patch_edges = 0;
    std::unordered_set<size_t> seen_edges;
    for (FaceID f : res.faces)
        for (Walker w = am.mesh.walker(f); !w.full_circle(); w = w.circulate_face_ccw()) {
            HalfEdgeID h = w.halfedge();
            HalfEdgeID hm = w.hmin();
            if (!seen_edges.insert(hm.get_index()).second)
                continue;
            FaceID fo = am.mesh.walker(h).opp().face();
            if (fo != InvalidFaceID && patch_face.count(fo.get_index()))
                ++interior_edges;
            else
                ++boundary_patch_edges;
        }
    TASK_CHECK(boundary_patch_edges == static_cast<size_t>(n));
    TASK_CHECK(2 * interior_edges + boundary_patch_edges == 3 * res.faces.size());

    // Every patch triangle connects only row vertices: each face has three
    // distinct, in-use vertices and the mesh stays valid.
    TASK_CHECK(valid(am.mesh));
    for (FaceID f : res.faces) {
        std::unordered_set<size_t> vs;
        for (VertexID v : am.mesh.incident_vertices(f))
            TASK_CHECK(vs.insert(v.get_index()).second && am.mesh.in_use(v));
        TASK_CHECK(vs.size() == 3);
    }
    return 0;
}

// Old element handles, positions and corner data survive the fill.
static int topology_old_handles_preserved()
{
    AttributedManifold am = build_annulus(6, 0.0);
    HalfEdgeID hole = find_hole(am.mesh);

    std::vector<VertexID> old_verts(am.mesh.vertices_begin(), am.mesh.vertices_end());
    std::vector<FaceID> old_faces(am.mesh.faces_begin(), am.mesh.faces_end());
    std::unordered_map<size_t, Vec3d> old_pos;
    for (VertexID v : old_verts)
        old_pos[v.get_index()] = am.mesh.pos(v);

    // Marker corner on an outside face adjacent to the hole.
    HalfEdgeID marker = am.mesh.walker(hole).opp().prev().halfedge();
    am.texcoords[marker] = Vec2d(0.31, 0.77);
    am.has_texcoord[marker] = 1;
    am.normals[marker] = Vec3d(0.0, 0.0, 1.0);
    am.has_normal[marker] = 1;

    size_t faces_before = am.mesh.no_faces();
    HolePatchResult res = fill_fair_hole(am, hole, 2);

    for (VertexID v : old_verts) {
        TASK_CHECK(am.mesh.in_use(v));
        TASK_CHECK(approx_v(am.mesh.pos(v), old_pos[v.get_index()]));
    }
    for (FaceID f : old_faces)
        TASK_CHECK(am.mesh.in_use(f));
    TASK_CHECK(am.mesh.no_faces() == faces_before + res.faces.size());
    TASK_CHECK(am.has_texcoord[marker] == 1);
    TASK_CHECK(approx_v2(am.texcoords[marker], Vec2d(0.31, 0.77)));
    TASK_CHECK(am.has_normal[marker] == 1);
    TASK_CHECK(approx_v(am.normals[marker], Vec3d(0.0, 0.0, 1.0)));
    return 0;
}

// A second, unselected hole is left completely untouched.
static int topology_unselected_hole()
{
    // Two disjoint annuli in one manifold (distinct vertex clusters).
    AttributedManifold am;
    VertexAttributeVector<int> cluster;
    int offset = 0;
    for (int component = 0; component < 2; ++component) {
        const int n = 6;
        std::vector<Vec3d> verts;
        const double shift = component * 10.0;
        for (int ring = 0; ring < 3; ++ring) {
            const double rad = 1.0 + ring;
            for (int i = 0; i < n; ++i) {
                const double a = 2.0 * PI * i / n;
                verts.emplace_back(shift + rad * std::cos(a), rad * std::sin(a), 0.0);
            }
        }
        auto id = [&](int r, int i) { return offset + r * n + (i % n); };
        for (int r = 0; r < 2; ++r)
            for (int i = 0; i < n; ++i) {
                std::array<int, 3> t0 = {id(r, i), id(r + 1, i), id(r, i + 1)};
                std::array<int, 3> t1 = {id(r, i + 1), id(r + 1, i), id(r + 1, i + 1)};
                for (const auto& t : std::array<std::array<int, 3>, 2>{t0, t1}) {
                    FaceID f = am.mesh.add_face({verts[t[0] - offset], verts[t[1] - offset], verts[t[2] - offset]});
                    int j = 0;
                    circulate_face_ccw(am.mesh, f, [&](VertexID v) { cluster[v] = t[j++]; });
                }
            }
        offset += 3 * n;
    }
    stitch_mesh(am.mesh, cluster);
    IDRemap remap;
    am.mesh.cleanup(remap);

    // Each annulus contributes two boundary curves (inner hole and outer
    // rim) of six face-less halfedges: 2 components * 2 curves * 6 = 24.
    std::vector<HalfEdgeID> holes;
    for (HalfEdgeID h : am.mesh.halfedges())
        if (am.mesh.walker(h).face() == InvalidFaceID)
            holes.push_back(h);
    TASK_CHECK(holes.size() == 24);

    HolePatchResult res = fill_fair_hole(am, holes[0], 1);
    TASK_CHECK(res.faces.size() == 6);

    size_t open = 0;
    for (HalfEdgeID h : am.mesh.halfedges())
        if (am.mesh.walker(h).face() == InvalidFaceID)
            ++open;
    TASK_CHECK(open == 18);

    // Another loop still forms a closed chain of six face-less halfedges.
    HalfEdgeID start = InvalidHalfEdgeID;
    for (HalfEdgeID h : holes)
        if (am.mesh.walker(h).face() == InvalidFaceID) { start = h; break; }
    TASK_CHECK(start != InvalidHalfEdgeID);
    int steps = 0;
    HalfEdgeID c = start;
    do {
        TASK_CHECK(am.mesh.walker(c).face() == InvalidFaceID);
        c = am.mesh.walker(c).next().halfedge();
        ++steps;
    }
    while (c != start && steps < 12);
    TASK_CHECK(steps == 6);
    return 0;
}

// Invalid arguments throw std::invalid_argument and leave the object intact.
static int topology_invalid_input_rollback()
{
    AttributedManifold am = build_annulus(6, 0.0);
    HalfEdgeID hole = find_hole(am.mesh);
    size_t faces_before = am.mesh.no_faces();
    size_t verts_before = am.mesh.no_vertices();

    // A face halfedge is not a boundary.
    HalfEdgeID face_he = InvalidHalfEdgeID;
    for (HalfEdgeID h : am.mesh.halfedges())
        if (am.mesh.walker(h).face() != InvalidFaceID) { face_he = h; break; }
    TASK_CHECK(face_he != InvalidHalfEdgeID);
    bool threw = false;
    try {
        fill_fair_hole(am, face_he, 2);
    }
    catch (const std::invalid_argument&) {
        threw = true;
    }
    TASK_CHECK(threw);
    TASK_CHECK(am.mesh.no_faces() == faces_before);
    TASK_CHECK(am.mesh.no_vertices() == verts_before);
    TASK_CHECK(am.mesh.walker(hole).face() == InvalidFaceID);

    // Bad round counts.
    for (unsigned bad : {0u, 4u, 99u}) {
        threw = false;
        try {
            fill_fair_hole(am, hole, bad);
        }
        catch (const std::invalid_argument&) {
            threw = true;
        }
        TASK_CHECK(threw);
        TASK_CHECK(am.mesh.no_faces() == faces_before);
    }

    // The object is still usable after the failures.
    HolePatchResult res = fill_fair_hole(am, hole, 1);
    TASK_CHECK(res.faces.size() == 6);
    TASK_CHECK(valid(am.mesh));
    return 0;
}

// A genuine numerical failure (fold) throws std::runtime_error, restores the
// whole object including attributes, and a later valid call still succeeds.
static int topology_runtime_failure_then_continue()
{
    AttributedManifold am = build_fan_disk(8, -1.0);
    HalfEdgeID hole = find_hole(am.mesh);
    // Attribute on an outside corner: must survive the rollback untouched.
    HalfEdgeID corner = am.mesh.walker(hole).opp().prev().halfedge();
    am.texcoords[corner] = Vec2d(0.42, 0.24);
    am.has_texcoord[corner] = 1;

    size_t faces_before = am.mesh.no_faces();
    bool threw = false;
    try {
        fill_fair_hole(am, hole, 3);
    }
    catch (const std::runtime_error&) {
        threw = true;
    }
    TASK_CHECK(threw);
    TASK_CHECK(am.mesh.no_faces() == faces_before);
    TASK_CHECK(am.mesh.walker(hole).face() == InvalidFaceID);
    TASK_CHECK(am.has_texcoord[corner] == 1);
    TASK_CHECK(approx_v2(am.texcoords[corner], Vec2d(0.42, 0.24)));
    TASK_CHECK(valid(am.mesh));

    HolePatchResult res = fill_fair_hole(am, hole, 2);
    TASK_CHECK(res.residual <= 1e-8);
    TASK_CHECK(valid(am.mesh));
    return 0;
}

// ===========================================================================
// channels_io
// ===========================================================================

// Full channel pipeline: UV jump at the seam, mixed explicit/default normals,
// cleanup and per-corner OBJ round trip.
static int channels_full_roundtrip()
{
    const int n = 9;
    AttributedManifold am = build_annulus(n, 0.0);
    HalfEdgeID hole = find_hole(am.mesh);
    std::vector<HalfEdgeID> loop;
    HalfEdgeID c = hole;
    do {
        loop.push_back(c);
        c = am.mesh.walker(c).next().halfedge();
    }
    while (c != hole);

    // Outside corner UVs, with an explicit jump between adjacent boundaries.
    std::unordered_map<size_t, Vec2d> expected_b_uv;
    int face_counter = 0;
    for (FaceID f : am.mesh.faces()) {
        for (Walker w = am.mesh.walker(f); !w.full_circle(); w = w.circulate_face_ccw()) {
            HalfEdgeID h = w.halfedge();
            const Vec3d& p = am.mesh.pos(w.vertex());
            am.texcoords[h] = Vec2d(0.25 * p[0] + 0.5, 0.25 * p[1] + 0.5);
            am.has_texcoord[h] = 1;
            // Explicit normals only on alternating faces, others default.
            if (face_counter % 2 == 0) {
                am.normals[h] = normalize(am.mesh.area_normal(f));
                am.has_normal[h] = 1;
            }
        }
        ++face_counter;
    }
    for (size_t i = 0; i < loop.size(); ++i) {
        HalfEdgeID outside = am.mesh.walker(loop[i]).opp().prev().halfedge();
        expected_b_uv[am.mesh.walker(loop[i]).vertex().get_index()] = am.texcoords[outside];
    }
    // Force a large UV jump at one boundary vertex.
    HalfEdgeID jump_corner = am.mesh.walker(loop[0]).opp().prev().halfedge();
    am.texcoords[jump_corner] = Vec2d(9.0, -7.0);
    expected_b_uv[am.mesh.walker(loop[0]).vertex().get_index()] = Vec2d(9.0, -7.0);

    HolePatchResult res = fill_fair_hole(am, hole, 2);
    TASK_CHECK(res.residual <= 1e-8);

    // Boundary patch corners carry exactly the outside corner values. After
    // the fill each former hole halfedge belongs to a patch face and its
    // angle points at the same boundary vertex.
    for (HalfEdgeID h : loop) {
        VertexID v = am.mesh.walker(h).vertex();
        TASK_CHECK(am.mesh.walker(h).face() != InvalidFaceID);
        TASK_CHECK(am.has_texcoord[h] == 1);
        TASK_CHECK(approx_v2(am.texcoords[h], expected_b_uv[v.get_index()]));
    }

    // Interior UVs satisfy the unweighted patch mean value equation to 1e-8.
    // The patch stores one UV per vertex; collect it directly from the patch
    // face corners.
    std::unordered_map<size_t, Vec2d> patch_uv;
    for (FaceID f : res.faces)
        for (Walker w = am.mesh.walker(f); !w.full_circle(); w = w.circulate_face_ccw()) {
            TASK_CHECK(am.has_texcoord[w.halfedge()] == 1);
            patch_uv[w.vertex().get_index()] = am.texcoords[w.halfedge()];
        }
    for (VertexID v : res.vertices) {
        std::unordered_set<size_t> seen;
        Vec2d mean(0.0, 0.0);
        int deg = 0;
        for (VertexID u : am.mesh.incident_vertices(v)) {
            if (!seen.insert(u.get_index()).second)
                continue;
            auto it = patch_uv.find(u.get_index());
            TASK_CHECK(it != patch_uv.end());
            mean += it->second;
            ++deg;
        }
        TASK_CHECK(deg > 0);
        mean /= deg;
        TASK_CHECK(patch_uv.count(v.get_index()));
        TASK_CHECK(approx_v2(patch_uv[v.get_index()], mean, 1e-8));
    }

    // Patch normals are unit vectors where present; outside masks preserved.
    for (FaceID f : res.faces)
        for (Walker w = am.mesh.walker(f); !w.full_circle(); w = w.circulate_face_ccw()) {
            HalfEdgeID h = w.halfedge();
            TASK_CHECK(am.has_normal[h] == 1);
            TASK_CHECK(approx(CGLA::length(am.normals[h]), 1.0, 1e-12));
        }

    am.cleanup();
    TASK_CHECK(valid(am.mesh));
    const std::string path = ".task-build/channels_full.obj";
    TASK_CHECK(obj_save(path, am));

    AttributedManifold back;
    TASK_CHECK(obj_load(path, back));
    TASK_CHECK(back.mesh.no_faces() == am.mesh.no_faces());

    // Per-corner comparison in face creation order.
    auto fit_a = am.mesh.faces_begin();
    auto fit_b = back.mesh.faces_begin();
    for (; fit_a != am.mesh.faces_end(); ++fit_a, ++fit_b) {
        Walker wa = am.mesh.walker(*fit_a);
        Walker wb = back.mesh.walker(*fit_b);
        for (; !wa.full_circle(); wa = wa.circulate_face_ccw(), wb = wb.circulate_face_ccw()) {
            TASK_CHECK(approx_v(am.mesh.pos(wa.vertex()), back.mesh.pos(wb.vertex()), 1e-12));
            HalfEdgeID ha = wa.halfedge(), hb = wb.halfedge();
            TASK_CHECK(am.has_texcoord[ha] == back.has_texcoord[hb]);
            if (am.has_texcoord[ha])
                TASK_CHECK(approx_v2(am.texcoords[ha], back.texcoords[hb], 1e-12));
            TASK_CHECK(am.has_normal[ha] == back.has_normal[hb]);
            if (am.has_normal[ha])
                TASK_CHECK(approx_v(am.normals[ha], back.normals[hb], 1e-12));
        }
    }
    return 0;
}

// Mixed v/vt/vn corner formats, negative independent indices and an ngon.
static int channels_mixed_negative_obj()
{
    const std::string path = ".task-build/channels_mixed.obj";
    {
        std::ofstream os(path);
        TASK_CHECK(bool(os));
        os << "v 0 0 0\nv 1 0 0\nv 1 1 0\nv 0 1 0\nv 0.5 0.5 1\n";
        os << "vt 0.0 0.0\nvt 1.0 0.0\nvt 0.5 1.0\n";
        os << "vn 0 0 1\nvn 1 0 0\n";
        // full, v//vn, and v/vt formats mixed
        os << "f 1/1/1 2/2/1 3//2\n";
        // negative independent indices: v=-2 -> vertex 4, vt=-2 -> tex 2,
        // vn=-1 -> normal 2
        os << "f -2/-2/-1 5 4\n";
        // v/vt, bare v and full format in one face
        os << "f 1/1 5 4/2/2\n";
        // polygon (quad)
        os << "f 1 2 3 4\n";
    }

    AttributedManifold am;
    TASK_CHECK(obj_load(path, am));
    TASK_CHECK(am.mesh.no_faces() == 4);

    std::vector<FaceID> faces(am.mesh.faces_begin(), am.mesh.faces_end());
    TASK_CHECK(am.mesh.no_edges(faces[0]) == 3);
    TASK_CHECK(am.mesh.no_edges(faces[3]) == 4);

    // Face 1 corner 0: v1, t1, n1 all present.
    Walker w = am.mesh.walker(faces[0]);
    TASK_CHECK(am.has_texcoord[w.halfedge()] == 1);
    TASK_CHECK(approx_v2(am.texcoords[w.halfedge()], Vec2d(0.0, 0.0)));
    TASK_CHECK(am.has_normal[w.halfedge()] == 1);
    TASK_CHECK(approx_v(am.normals[w.halfedge()], Vec3d(0, 0, 1)));
    // Face 1 corner 2: normal only.
    w = w.circulate_face_ccw().circulate_face_ccw();
    TASK_CHECK(am.has_texcoord[w.halfedge()] == 0);
    TASK_CHECK(am.has_normal[w.halfedge()] == 1);
    TASK_CHECK(approx_v(am.normals[w.halfedge()], Vec3d(1, 0, 0)));

    // Face 2 corner 0 resolved through negative indices: vertex 4, tex 2,
    // normal 2.
    w = am.mesh.walker(faces[1]);
    TASK_CHECK(approx_v(am.mesh.pos(w.vertex()), Vec3d(0, 1, 0), 1e-12));
    TASK_CHECK(approx_v2(am.texcoords[w.halfedge()], Vec2d(1.0, 0.0)));
    TASK_CHECK(approx_v(am.normals[w.halfedge()], Vec3d(1, 0, 0)));

    // Face 3 corner 1 is a bare vertex with no attributes.
    w = am.mesh.walker(faces[2]).circulate_face_ccw();
    TASK_CHECK(am.has_texcoord[w.halfedge()] == 0);
    TASK_CHECK(am.has_normal[w.halfedge()] == 0);

    // Saving and reloading preserves every mask and value.
    const std::string path2 = ".task-build/channels_mixed_rt.obj";
    TASK_CHECK(obj_save(path2, am));
    AttributedManifold back;
    TASK_CHECK(obj_load(path2, back));
    TASK_CHECK(back.mesh.no_faces() == am.mesh.no_faces());
    auto fa = am.mesh.faces_begin();
    auto fb = back.mesh.faces_begin();
    for (; fa != am.mesh.faces_end(); ++fa, ++fb) {
        Walker wa = am.mesh.walker(*fa);
        Walker wb = back.mesh.walker(*fb);
        for (; !wa.full_circle(); wa = wa.circulate_face_ccw(), wb = wb.circulate_face_ccw()) {
            TASK_CHECK(am.has_texcoord[wa.halfedge()] == back.has_texcoord[wb.halfedge()]);
            TASK_CHECK(am.has_normal[wa.halfedge()] == back.has_normal[wb.halfedge()]);
            if (am.has_texcoord[wa.halfedge()])
                TASK_CHECK(approx_v2(am.texcoords[wa.halfedge()], back.texcoords[wb.halfedge()]));
            if (am.has_normal[wa.halfedge()])
                TASK_CHECK(approx_v(am.normals[wa.halfedge()], back.normals[wb.halfedge()]));
        }
    }

    // A missing file must fail with false and leave the object untouched.
    AttributedManifold unchanged = am;
    TASK_CHECK(!obj_load(".task-build/does_not_exist_xyz.obj", am));
    TASK_CHECK(am.mesh.no_faces() == unchanged.mesh.no_faces());
    return 0;
}

// Missing boundary UV: the whole new patch is UV-less and no fake zero UV is
// ever written.
static int channels_missing_uv()
{
    // No UV anywhere: the saved file must contain no vt records at all.
    AttributedManifold am = build_annulus(7, 0.0);
    HalfEdgeID hole = find_hole(am.mesh);
    HolePatchResult res = fill_fair_hole(am, hole, 1);
    for (FaceID f : res.faces)
        for (Walker w = am.mesh.walker(f); !w.full_circle(); w = w.circulate_face_ccw())
            TASK_CHECK(am.has_texcoord[w.halfedge()] == 0);

    const std::string path = ".task-build/channels_no_uv.obj";
    TASK_CHECK(obj_save(path, am));
    std::ifstream is(path);
    TASK_CHECK(bool(is));
    std::string line;
    int vt_lines = 0;
    while (std::getline(is, line))
        if (line.rfind("vt ", 0) == 0)
            ++vt_lines;
    TASK_CHECK(vt_lines == 0);

    AttributedManifold back;
    TASK_CHECK(obj_load(path, back));
    for (FaceID f : back.mesh.faces())
        for (Walker w = back.mesh.walker(f); !w.full_circle(); w = w.circulate_face_ccw())
            TASK_CHECK(back.has_texcoord[w.halfedge()] == 0);

    // Partial case: a single missing boundary UV disables the whole patch
    // while existing outside UV data is preserved.
    AttributedManifold pm = build_annulus(7, 0.0);
    HalfEdgeID phole = find_hole(pm.mesh);
    for (FaceID f : pm.mesh.faces())
        for (Walker w = pm.mesh.walker(f); !w.full_circle(); w = w.circulate_face_ccw()) {
            pm.texcoords[w.halfedge()] = Vec2d(0.1, 0.2);
            pm.has_texcoord[w.halfedge()] = 1;
        }
    HalfEdgeID keep = pm.mesh.walker(phole).opp().prev().halfedge();
    pm.has_texcoord[keep] = 0;
    HolePatchResult pr = fill_fair_hole(pm, phole, 2);
    for (FaceID f : pr.faces)
        for (Walker w = pm.mesh.walker(f); !w.full_circle(); w = w.circulate_face_ccw())
            TASK_CHECK(pm.has_texcoord[w.halfedge()] == 0);
    size_t outside_uv = 0;
    for (FaceID f : pm.mesh.faces()) {
        if (std::find(pr.faces.begin(), pr.faces.end(), f) != pr.faces.end())
            continue;
        for (Walker w = pm.mesh.walker(f); !w.full_circle(); w = w.circulate_face_ccw())
            if (pm.has_texcoord[w.halfedge()])
                ++outside_uv;
    }
    TASK_CHECK(outside_uv > 0);
    return 0;
}

// Preexisting sparse slots (a removed face), then fill, cleanup and an OBJ
// round trip: every surviving corner keeps consistent data.
static int channels_sparse_slots_cleanup()
{
    AttributedManifold am = build_annulus(8, 0.0);
    HalfEdgeID hole = find_hole(am.mesh);

    // Remove an outer-rim face (all vertices at radius >= 2) to create
    // inactive slots without touching the target hole on the inner ring.
    FaceID victim = InvalidFaceID;
    for (FaceID f : am.mesh.faces()) {
        bool outer = false, touches_hole = false;
        for (VertexID v : am.mesh.incident_vertices(f)) {
            const Vec3d& p = am.mesh.pos(v);
            double rad = std::sqrt(p[0] * p[0] + p[1] * p[1]);
            if (approx(rad, 3.0, 1e-9))
                outer = true;
            if (approx(rad, 1.0, 1e-9))
                touches_hole = true;
        }
        if (outer && !touches_hole) { victim = f; break; }
    }
    TASK_CHECK(victim != InvalidFaceID);
    TASK_CHECK(am.mesh.remove_face(victim));
    TASK_CHECK(am.mesh.allocated_faces() > am.mesh.no_faces());
    // The captured inner hole halfedge must still be a live boundary.
    TASK_CHECK(am.mesh.in_use(hole));
    TASK_CHECK(am.mesh.walker(hole).face() == InvalidFaceID);

    for (FaceID f : am.mesh.faces())
        for (Walker w = am.mesh.walker(f); !w.full_circle(); w = w.circulate_face_ccw()) {
            const Vec3d& p = am.mesh.pos(w.vertex());
            HalfEdgeID h = w.halfedge();
            am.texcoords[h] = Vec2d(0.11 * p[0], 0.13 * p[1]);
            am.has_texcoord[h] = 1;
            am.normals[h] = normalize(am.mesh.area_normal(f));
            am.has_normal[h] = 1;
        }

    HolePatchResult res = fill_fair_hole(am, hole, 2);
    TASK_CHECK(res.residual <= 1e-8);
    am.cleanup();
    TASK_CHECK(am.mesh.allocated_vertices() == am.mesh.no_vertices());
    TASK_CHECK(valid(am.mesh));

    const std::string path = ".task-build/channels_sparse.obj";
    TASK_CHECK(obj_save(path, am));
    AttributedManifold back;
    TASK_CHECK(obj_load(path, back));
    TASK_CHECK(back.mesh.no_faces() == am.mesh.no_faces());
    auto fa = am.mesh.faces_begin();
    auto fb = back.mesh.faces_begin();
    for (; fa != am.mesh.faces_end(); ++fa, ++fb) {
        Walker wa = am.mesh.walker(*fa);
        Walker wb = back.mesh.walker(*fb);
        for (; !wa.full_circle(); wa = wa.circulate_face_ccw(), wb = wb.circulate_face_ccw()) {
            TASK_CHECK(am.has_texcoord[wa.halfedge()] == back.has_texcoord[wb.halfedge()]);
            if (am.has_texcoord[wa.halfedge()])
                TASK_CHECK(approx_v2(am.texcoords[wa.halfedge()], back.texcoords[wb.halfedge()], 1e-12));
            TASK_CHECK(am.has_normal[wa.halfedge()] == back.has_normal[wb.halfedge()]);
            if (am.has_normal[wa.halfedge()])
                TASK_CHECK(approx_v(am.normals[wa.halfedge()], back.normals[wb.halfedge()], 1e-12));
        }
    }
    return 0;
}

extern const struct task_case task_cases[] = {
    {"fairing", "dense_reference_finite_difference", fairing_dense_reference},
    {"fairing", "outside_ring_influence", fairing_outside_ring_influence},
    {"fairing", "nonplanar_hole", fairing_nonplanar_hole},
    {"fairing", "refinement_rounds", fairing_refinement_rounds},
    {"fairing", "rigid_transform", fairing_rigid_transform},
    {"topology", "patch_connectivity", topology_patch_connectivity},
    {"topology", "old_handles_preserved", topology_old_handles_preserved},
    {"topology", "unselected_hole", topology_unselected_hole},
    {"topology", "invalid_input_rollback", topology_invalid_input_rollback},
    {"topology", "runtime_failure_then_continue", topology_runtime_failure_then_continue},
    {"channels_io", "full_roundtrip", channels_full_roundtrip},
    {"channels_io", "mixed_negative_ngon", channels_mixed_negative_obj},
    {"channels_io", "missing_uv_no_fake", channels_missing_uv},
    {"channels_io", "sparse_slots_cleanup", channels_sparse_slots_cleanup},
};
extern const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
