/* ----------------------------------------------------------------------- *
 * This file is part of GEL, http://www.imm.dtu.dk/GEL
 * Copyright (C) the authors and DTU Informatics
 * For license and list of authors, see ../../doc/intro.pdf
 * ----------------------------------------------------------------------- */

#include <GEL/HMesh/HolePatch.h>

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace HMesh
{
    using namespace CGLA;
    using namespace std;

    namespace
    {
        constexpr double kGradTol = 1e-8;
        constexpr double kCgTol = 1e-12;

        bool finite_vec(const Vec3d& v)
        {
            return isfinite(v[0]) && isfinite(v[1]) && isfinite(v[2]);
        }

        // Real vertex neighbors of v in the patched mesh, deduplicated.
        vector<VertexID> neighbors_of(const Manifold& m, VertexID v)
        {
            vector<VertexID> out;
            unordered_set<size_t> seen;
            for (VertexID u : m.incident_vertices(v))
                if (seen.insert(u.get_index()).second)
                    out.push_back(u);
            return out;
        }

        // Sparse symmetric matrix stored as, per row, a list of
        // (column, value) entries (the diagonal entry included).
        struct SparseMatrix
        {
            vector<vector<pair<size_t, double>>> rows;
            explicit SparseMatrix(size_t n) : rows(n) {}
            void add(size_t r, size_t c, double v) { rows[r].push_back({c, v}); }
            vector<double> multiply(const vector<double>& x) const
            {
                vector<double> y(rows.size(), 0.0);
                for (size_t r = 0; r < rows.size(); ++r)
                    for (const auto& [c, v] : rows[r])
                        y[r] += v * x[c];
                return y;
            }
        };

        // Conjugate gradient solve of A x = b for one scalar coordinate.
        // Returns the max absolute residual |A x - b|.
        double cg_scalar(const SparseMatrix& A,
                         const vector<double>& rhs,
                         vector<double>& x,
                         size_t max_iters)
        {
            const size_t n = x.size();
            vector<double> r(n, 0.0), p(n, 0.0), ap(n, 0.0);
            vector<double> ax = A.multiply(x);
            for (size_t i = 0; i < n; ++i) {
                r[i] = rhs[i] - ax[i];
                p[i] = r[i];
            }
            double rr = 0.0;
            for (double v : r)
                rr += v * v;
            double inf_r = 0.0;
            for (double v : r)
                inf_r = max(inf_r, fabs(v));
            if (inf_r <= kCgTol)
                return inf_r;

            for (size_t it = 0; it < max_iters && rr > 0.0; ++it) {
                ap = A.multiply(p);
                double pap = 0.0;
                for (size_t i = 0; i < n; ++i)
                    pap += p[i] * ap[i];
                if (pap == 0.0)
                    break;
                double alpha = rr / pap;
                double rinf = 0.0;
                for (size_t i = 0; i < n; ++i) {
                    x[i] += alpha * p[i];
                    r[i] -= alpha * ap[i];
                    rinf = max(rinf, fabs(r[i]));
                }
                double rr_new = 0.0;
                for (double v : r)
                    rr_new += v * v;
                double beta = rr_new / rr;
                for (size_t i = 0; i < n; ++i)
                    p[i] = r[i] + beta * p[i];
                rr = rr_new;
                inf_r = rinf;
                if (inf_r <= kCgTol)
                    break;
            }
            return inf_r;
        }

        // Mean value equation solution for the interior UVs, with boundary
        // UVs held fixed. Adjacency is the patch graph only.
        bool fair_uvs(const vector<VertexID>& boundary_verts,
                      const vector<VertexID>& interior_verts,
                      const unordered_map<size_t, size_t>& global_index,
                      const vector<vector<size_t>>& patch_adj,
                      vector<Vec2d>& uv)
        {
            const size_t nb = boundary_verts.size();
            const size_t ni = interior_verts.size();

            // Free-node data: patch neighbors split into free interior
            // nodes and fixed (boundary) nodes.
            vector<vector<size_t>> free_adj(ni);
            vector<Vec2d> fixed_sum(ni, Vec2d(0.0, 0.0));
            vector<size_t> degree(ni, 0);
            for (size_t a = 0; a < ni; ++a) {
                size_t gi = nb + a;
                degree[a] = patch_adj[gi].size();
                for (size_t gj : patch_adj[gi]) {
                    if (gj >= nb)
                        free_adj[a].push_back(gj - nb);
                    else
                        fixed_sum[a] += uv[gj];
                }
            }

            // Interior mean-value system: deg on the diagonal, -1 per free
            // neighbor, fixed boundary contributions on the right hand side.
            SparseMatrix A(ni);
            for (size_t a = 0; a < ni; ++a) {
                A.add(a, a, static_cast<double>(degree[a]));
                for (size_t b : free_adj[a])
                    A.add(a, b, -1.0);
            }

            vector<Vec2d> x(ni, Vec2d(0.0, 0.0));
            double worst = 0.0;
            for (int coord = 0; coord < 2; ++coord) {
                vector<double> xc(ni, 0.0), rhs(ni);
                for (size_t a = 0; a < ni; ++a)
                    rhs[a] = fixed_sum[a][coord];
                double res = cg_scalar(A, rhs, xc, 2 * ni + 64);
                worst = max(worst, res);
                for (size_t a = 0; a < ni; ++a)
                    x[a][coord] = xc[a];
            }
            if (worst > kCgTol)
                return false;
            for (size_t a = 0; a < ni; ++a)
                uv[nb + a] = x[a];
            return true;
        }
    }

    HolePatchResult fill_fair_hole(AttributedManifold& am, HalfEdgeID boundary,
                                   unsigned refinement_rounds)
    {
        // Full deep copy: the implicit copy preserves inactive slots and
        // element IDs, so any failure restores the exact original object.
        AttributedManifold backup = am;
        Manifold& m = am.mesh;

        try {
            if (refinement_rounds < 1 || refinement_rounds > 3)
                throw invalid_argument("refinement_rounds must be between 1 and 3");
            if (!m.in_use(boundary) || m.walker(boundary).face() != InvalidFaceID)
                throw invalid_argument("boundary must be an active face-less halfedge");

            // Collect the complete boundary loop.
            vector<HalfEdgeID> loop;
            vector<VertexID> boundary_verts;
            {
                HalfEdgeID h = boundary;
                unordered_set<size_t> seen_verts;
                size_t guard = m.allocated_halfedges() + 1;
                do {
                    if (m.walker(h).face() != InvalidFaceID)
                        throw invalid_argument("boundary loop contains a face halfedge");
                    if (m.walker(h).opp().face() == InvalidFaceID)
                        throw invalid_argument("boundary halfedge is not adjacent to an outside face");
                    VertexID v = m.walker(h).vertex();
                    if (!seen_verts.insert(v.get_index()).second)
                        throw invalid_argument("hole boundary is not simple");
                    loop.push_back(h);
                    boundary_verts.push_back(v);
                    h = m.walker(h).next().halfedge();
                }
                while (h != boundary && --guard > 0);
                if (h != boundary)
                    throw invalid_argument("boundary halfedge does not lie on a closed boundary loop");
            }
            const size_t n = loop.size();
            if (n < 3)
                throw invalid_argument("hole must have at least three edges");

            // Finite coordinates, positive edge lengths and face areas over
            // the whole input mesh.
            for (VertexID v : m.vertices())
                if (!finite_vec(m.pos(v)))
                    throw invalid_argument("mesh contains non-finite coordinates");
            for (HalfEdgeID h : m.halfedges()) {
                if (m.walker(h).face() == InvalidFaceID)
                    continue;
                if (m.length(h) <= 0.0)
                    throw invalid_argument("mesh contains a non-positive edge length");
            }
            for (FaceID f : m.faces()) {
                if (m.area(f) <= 0.0)
                    throw invalid_argument("mesh contains a non-positive face area");
            }

            // Newell normal of the boundary polygon.
            Vec3d newell(0.0);
            for (size_t i = 0; i < n; ++i)
                newell += cross(m.pos(boundary_verts[i]),
                                m.pos(boundary_verts[(i + 1) % n]));
            if (sqr_length(newell) <= 0.0)
                throw invalid_argument("boundary Newell normal is zero");
            Vec3d plane_n = normalize(newell);

            // Orthonormal plane basis.
            Vec3d axis = fabs(plane_n[0]) > fabs(plane_n[1])
                             ? (fabs(plane_n[0]) > fabs(plane_n[2]) ? Vec3d(1, 0, 0) : Vec3d(0, 0, 1))
                             : (fabs(plane_n[1]) > fabs(plane_n[2]) ? Vec3d(0, 1, 0) : Vec3d(0, 0, 1));
            Vec3d e1 = normalize(cross(plane_n, axis));
            Vec3d e2 = cross(plane_n, e1);

            // Strict convexity of the projected polygon.
            vector<Vec2d> q(n);
            for (size_t i = 0; i < n; ++i)
                q[i] = Vec2d(dot(m.pos(boundary_verts[i]), e1),
                             dot(m.pos(boundary_verts[i]), e2));
            int turn_sign = 0;
            for (size_t i = 0; i < n; ++i) {
                Vec2d d0 = q[i] - q[(i + n - 1) % n];
                Vec2d d1 = q[(i + 1) % n] - q[i];
                double c = cross(d0, d1);
                if (c == 0.0)
                    throw invalid_argument("projected hole is not strictly convex");
                int s = c > 0.0 ? 1 : -1;
                if (turn_sign == 0)
                    turn_sign = s;
                else if (turn_sign != s)
                    throw invalid_argument("projected hole is not convex");
            }

            // Outside corner halfedge of every boundary vertex, captured
            // before the hole is touched.
            vector<HalfEdgeID> outside_corner(n);
            for (size_t i = 0; i < n; ++i) {
                HalfEdgeID c = m.walker(loop[i]).opp().prev().halfedge();
                if (m.walker(c).face() == InvalidFaceID)
                    throw invalid_argument("boundary vertex lacks an outside face corner");
                outside_corner[i] = c;
            }

            // ---- Topology: cap then barycentric triangle splits ----------
            FaceID cap = m.close_hole(loop[0]);
            if (!m.in_use(cap))
                throw runtime_error("close_hole failed to produce a cap face");

            vector<VertexID> new_vertices;
            VertexID first_center = m.split_face_by_vertex(cap);
            new_vertices.push_back(first_center);

            vector<FaceID> patch_faces;
            for (FaceID f : m.incident_faces(first_center))
                patch_faces.push_back(f);
            if (patch_faces.size() != n)
                throw runtime_error("first refinement did not produce one triangle per hole edge");

            for (unsigned round = 1; round < refinement_rounds; ++round) {
                vector<FaceID> next_faces;
                next_faces.reserve(patch_faces.size() * 3);
                for (FaceID f : patch_faces) {
                    VertexID c = m.split_face_by_vertex(f);
                    new_vertices.push_back(c);
                    for (FaceID nf : m.incident_faces(c))
                        next_faces.push_back(nf);
                }
                if (next_faces.size() != patch_faces.size() * 3)
                    throw runtime_error("patch refinement produced an unexpected face count");
                patch_faces = std::move(next_faces);
            }

            const size_t expected_faces = n * static_cast<size_t>(pow(3.0, static_cast<int>(refinement_rounds) - 1));
            if (patch_faces.size() != expected_faces)
                throw runtime_error("patch face count mismatch");

            // ---- Sparse adjacency of B union I --------------------------
            const size_t nb = boundary_verts.size();
            const size_t ni = new_vertices.size();
            const size_t ns = nb + ni;

            unordered_map<size_t, size_t> global_index;
            global_index.reserve(ns * 2);
            for (size_t i = 0; i < ns; ++i) {
                VertexID v = i < nb ? boundary_verts[i] : new_vertices[i - nb];
                global_index[v.get_index()] = i;
            }

            // Real adjacency of every row vertex.
            vector<vector<VertexID>> real_adj(ns);
            // Patch-only adjacency (neighbors that are themselves row vertices).
            vector<vector<size_t>> patch_adj(ns);
            for (size_t i = 0; i < ns; ++i) {
                VertexID v = i < nb ? boundary_verts[i] : new_vertices[i - nb];
                real_adj[i] = neighbors_of(m, v);
                for (VertexID u : real_adj[i]) {
                    auto it = global_index.find(u.get_index());
                    if (it != global_index.end())
                        patch_adj[i].push_back(it->second);
                }
            }

            // ---- Position solve: free interior vertices -----------------
            // E = sum over rows r in S of ||Dinv L_S p_S||^2, where L is the
            // graph Laplacian (deg on the diagonal, -1 per undirected edge)
            // and Dinv holds 1/deg. Only the interior positions x are free;
            // the boundary and outside positions are fixed. The normal
            // equations are (Dinv L_I)^T (Dinv L_I) x =
            // -(Dinv L_I)^T (Dinv L_F p_F). Everything is assembled from the
            // sparse per-row adjacency, never from a dense global matrix.
            vector<double> invdeg(ns);
            for (size_t r = 0; r < ns; ++r)
                invdeg[r] = 1.0 / static_cast<double>(real_adj[r].size());

            // Sparse rows W[r] = invdeg(r) * L[row r] restricted to free
            // (interior) columns, and q[r] = invdeg(r) * fixed part of L p.
            // Every row r in B union I participates: the boundary rows carry
            // free columns through their interior neighbors, which is exactly
            // how the outside one ring influences the solution.
            struct WRow
            {
                vector<pair<size_t, double>> entries; // free local column, weight
                Vec3d q;
            };
            vector<WRow> wrows(ns);
            for (size_t r = 0; r < ns; ++r) {
                Vec3d fixed_contrib(0.0);
                // Diagonal term deg(r) p_r: free for interior, fixed for B.
                if (r >= nb)
                    wrows[r].entries.push_back({r - nb, invdeg[r] * static_cast<double>(real_adj[r].size())});
                else
                    fixed_contrib += static_cast<double>(real_adj[r].size()) * m.pos(boundary_verts[r]);
                for (VertexID u : real_adj[r]) {
                    auto it = global_index.find(u.get_index());
                    if (it != global_index.end() && it->second >= nb)
                        wrows[r].entries.push_back({it->second - nb, -invdeg[r]});
                    else
                        fixed_contrib -= m.pos(u);
                }
                wrows[r].q = invdeg[r] * fixed_contrib;
            }

            // A = W^T W, rhs = -W^T q.
            vector<unordered_map<size_t, double>> adense(ni);
            vector<Vec3d> rhs_vec(ni, Vec3d(0.0));
            for (const WRow& wr : wrows) {
                for (const auto& [ca, wa] : wr.entries) {
                    for (const auto& [cb, wb] : wr.entries)
                        adense[ca][cb] += wa * wb;
                    rhs_vec[ca] -= wa * wr.q;
                }
            }
            SparseMatrix A(ni);
            for (size_t a = 0; a < ni; ++a) {
                for (auto& [b, v] : adense[a])
                    A.add(a, b, v);
            }

            vector<Vec3d> x(ni);
            for (size_t a = 0; a < ni; ++a)
                x[a] = m.pos(new_vertices[a]);

            double worst_res = 0.0;
            for (int coord = 0; coord < 3; ++coord) {
                vector<double> xc(ni), rhs(ni);
                for (size_t a = 0; a < ni; ++a) {
                    xc[a] = x[a][coord];
                    rhs[a] = rhs_vec[a][coord];
                }
                double res = cg_scalar(A, rhs, xc, 4 * ni + 128);
                worst_res = max(worst_res, res);
                for (size_t a = 0; a < ni; ++a)
                    x[a][coord] = xc[a];
            }
            if (worst_res > kCgTol || !isfinite(worst_res))
                throw runtime_error("fair hole linear solve did not converge");

            for (size_t a = 0; a < ni; ++a) {
                if (!finite_vec(x[a]))
                    throw runtime_error("fair hole solution is non-finite");
                m.pos(new_vertices[a]) = x[a];
            }

            // True gradient with respect to every new vertex coordinate:
            // grad = 2 L_I^T Dinv d, with d[r] = Dinv L_S p_S the laplacian
            // residual of every row in B union I.
            vector<Vec3d> drow(ns);
            for (size_t r = 0; r < ns; ++r) {
                Vec3d mean(0.0);
                for (VertexID u : real_adj[r])
                    mean += m.pos(u);
                mean *= invdeg[r];
                VertexID vr = r < nb ? boundary_verts[r] : new_vertices[r - nb];
                drow[r] = m.pos(vr) - mean;
            }
            vector<Vec3d> grad(ni, Vec3d(0.0));
            for (size_t r = 0; r < ns; ++r) {
                // diagonal term of L[r][c]
                if (r >= nb)
                    grad[r - nb] += static_cast<double>(real_adj[r].size()) * invdeg[r] * drow[r];
                // -1 off-diagonal terms of L[r][c] for free columns
                for (VertexID u : real_adj[r]) {
                    auto it = global_index.find(u.get_index());
                    if (it != global_index.end() && it->second >= nb)
                        grad[it->second - nb] -= invdeg[r] * drow[r];
                }
            }
            double residual = 0.0;
            for (size_t a = 0; a < ni; ++a)
                for (int c = 0; c < 3; ++c)
                    residual = max(residual, fabs(2.0 * grad[a][c]));
            if (residual > kGradTol || !isfinite(residual))
                throw runtime_error("fair hole gradient residual above tolerance");

            // Actual energy over B and I rows on the patched adjacency.
            double energy = 0.0;
            for (size_t r = 0; r < ns; ++r)
                energy += dot(drow[r], drow[r]);
            if (!isfinite(energy))
                throw runtime_error("fair hole energy is non-finite");

            // ---- Fold / degeneracy check in the Newell plane ------------
            // All patch triangles must have nonzero signed area with one
            // common sign: a fold produces opposite signs, a collapsed
            // triangle produces zero.
            int area_sign = 0;
            for (FaceID f : patch_faces) {
                vector<VertexID> fv;
                for (VertexID v : m.incident_vertices(f))
                    fv.push_back(v);
                if (fv.size() != 3)
                    throw runtime_error("patch contains a non-triangle face");
                Vec3d area_n = 0.5 * cross(m.pos(fv[1]) - m.pos(fv[0]),
                                           m.pos(fv[2]) - m.pos(fv[0]));
                double s = dot(area_n, plane_n);
                if (s == 0.0)
                    throw runtime_error("fair hole produced a degenerate triangle");
                int sgn = s > 0.0 ? 1 : -1;
                if (area_sign == 0)
                    area_sign = sgn;
                else if (area_sign != sgn)
                    throw runtime_error("fair hole unfolded: patch triangles have opposite orientations");
            }

            // ---- Texture coordinates ------------------------------------
            bool all_boundary_uv = true;
            vector<Vec2d> boundary_uv(nb);
            for (size_t i = 0; i < nb; ++i) {
                HalfEdgeID c = outside_corner[i];
                if (am.has_texcoord[c] == 0) {
                    all_boundary_uv = false;
                    break;
                }
                boundary_uv[i] = am.texcoords[c];
            }

            if (all_boundary_uv) {
                vector<Vec2d> uv(ns);
                for (size_t i = 0; i < nb; ++i)
                    uv[i] = boundary_uv[i];
                for (size_t a = 0; a < ni; ++a)
                    uv[nb + a] = Vec2d(0.0, 0.0);
                if (!fair_uvs(boundary_verts, new_vertices, global_index, patch_adj, uv))
                    throw runtime_error("patch UV solve did not converge");
                for (FaceID f : patch_faces)
                    for (Walker w = m.walker(f); !w.full_circle(); w = w.circulate_face_ccw()) {
                        HalfEdgeID h = w.halfedge();
                        auto it = global_index.find(w.vertex().get_index());
                        if (it == global_index.end())
                            throw runtime_error("patch corner vertex missing from patch row set");
                        am.texcoords[h] = uv[it->second];
                        am.has_texcoord[h] = 1;
                    }
            }
            else {
                // The whole new patch is marked UV-less; old data is kept.
                for (FaceID f : patch_faces)
                    for (Walker w = m.walker(f); !w.full_circle(); w = w.circulate_face_ccw())
                        am.has_texcoord[w.halfedge()] = 0;
            }

            // ---- Explicit patch normals ---------------------------------
            unordered_map<size_t, Vec3d> normal_sum;
            normal_sum.reserve(ns * 2);
            for (FaceID f : patch_faces) {
                Vec3d an = m.area_normal(f);
                for (VertexID v : m.incident_vertices(f)) {
                    // Vec3d defaults to NaN, so the entry must be created
                    // explicitly as a zero vector before accumulation.
                    auto [it, inserted] = normal_sum.try_emplace(v.get_index(), Vec3d(0.0));
                    it->second += an;
                }
            }
            unordered_map<size_t, Vec3d> patch_normal;
            patch_normal.reserve(normal_sum.size() * 2);
            for (auto& [key, sum] : normal_sum) {
                if (sqr_length(sum) > 0.0)
                    patch_normal[key] = normalize(sum);
                // A zero (cancelled) sum leaves the corner mask invalid.
            }
            for (FaceID f : patch_faces)
                for (Walker w = m.walker(f); !w.full_circle(); w = w.circulate_face_ccw()) {
                    HalfEdgeID h = w.halfedge();
                    auto it = patch_normal.find(w.vertex().get_index());
                    if (it != patch_normal.end()) {
                        am.normals[h] = it->second;
                        am.has_normal[h] = 1;
                    }
                    else
                        am.has_normal[h] = 0;
                }

            HolePatchResult result;
            result.vertices = new_vertices;
            result.faces = patch_faces;
            result.energy = energy;
            result.residual = residual;
            return result;
        }
        catch (const invalid_argument&) {
            am = backup;
            throw;
        }
        catch (const runtime_error&) {
            am = backup;
            throw;
        }
    }
}
