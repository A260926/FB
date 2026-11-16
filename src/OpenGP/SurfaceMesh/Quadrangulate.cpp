#ifndef OPENGP_QUADRANGULATE_CPP
#define OPENGP_QUADRANGULATE_CPP

#include <OpenGP/SurfaceMesh/Quadrangulate.h>
#include <cmath>
#include <algorithm>
#include <functional>
#include <queue>
#include <set>
#include <vector>

//=============================================================================
namespace OpenGP {
//=============================================================================

namespace {

typedef SurfaceMesh::Vertex   Vertex;
typedef SurfaceMesh::Halfedge Halfedge;
typedef SurfaceMesh::Edge     Edge;
typedef SurfaceMesh::Face     Face;

inline bool finite_vec(const Vec3& v)
{
    return std::isfinite(v[0]) && std::isfinite(v[1]) && std::isfinite(v[2]);
}

/// Maximum-cardinality matching of a general (possibly non-bipartite) graph
/// with Edmonds' blossom algorithm (BFS contraction), O(V^3).
/// Returns the mate of each node (-1 for unmatched nodes).
std::vector<int> maximum_matching(const std::vector<std::vector<int> >& graph)
{
    const int n = (int)graph.size();
    std::vector<int> match(n, -1), parent(n), root(n);
    std::vector<char> used(n), in_queue(n);
    std::queue<int> q;

    auto enqueue = [&](int v)
    {
        if (!in_queue[v]) { in_queue[v] = 1; q.push(v); }
    };

    std::function<int(int,int)> lca = [&](int a, int b) -> int
    {
        std::vector<char> seen(n, 0);
        while (true)
        {
            a = root[a];
            seen[a] = 1;
            if (match[a] == -1) break;
            a = parent[match[a]];
        }
        while (true)
        {
            b = root[b];
            if (seen[b]) return b;
            b = parent[match[b]];
        }
    };

    std::function<void(int,int,int)> mark_path = [&](int v, int base, int child)
    {
        while (root[v] != base)
        {
            used[root[v]] = used[root[match[v]]] = 1;
            parent[v] = child;
            child = match[v];
            v = parent[match[v]];
        }
    };

    auto find_path = [&](int s) -> int
    {
        std::fill(parent.begin(), parent.end(), -1);
        for (int i = 0; i < n; ++i) root[i] = i;
        std::fill(used.begin(), used.end(), 0);
        std::fill(in_queue.begin(), in_queue.end(), 0);
        while (!q.empty()) q.pop();

        used[s] = 1;
        enqueue(s);

        while (!q.empty())
        {
            int v = q.front(); q.pop();
            in_queue[v] = 0;   // free the slot so a later blossom contraction can re-enqueue it
            for (int to : graph[v])
            {
                if (root[v] == root[to] || match[v] == to)
                    continue;

                if (to == s ||
                    (match[to] != -1 && parent[match[to]] != -1))
                {
                    // blossom found: contract it and continue the BFS
                    int base = lca(v, to);
                    std::fill(used.begin(), used.end(), 0);
                    mark_path(v, base, to);
                    mark_path(to, base, v);
                    // every vertex absorbed into the blossom has to be
                    // (re)examined with its new base, including ones that
                    // were already visited, or edges leaving the contracted
                    // blossom would be missed
                    for (int i = 0; i < n; ++i)
                    {
                        if (used[root[i]])
                        {
                            root[i] = base;
                            enqueue(i);
                        }
                    }
                }
                else if (parent[to] == -1)
                {
                    parent[to] = v;
                    if (match[to] == -1)
                        return to;
                    to = match[to];
                    used[to] = 1;
                    enqueue(to);
                }
            }
        }
        return -1;
    };

    for (int i = 0; i < n; ++i)
    {
        if (match[i] != -1) continue;
        int v = find_path(i);
        while (v != -1)
        {
            int pv = parent[v];
            int ppv = (pv != -1) ? match[pv] : -1;
            match[v] = pv;
            if (pv != -1) match[pv] = v;
            v = ppv;
        }
    }
    return match;
}

/// Geometric and attribute compatibility test for one candidate pair.
bool pair_is_quad(SurfaceMesh& mesh, Edge e, Face f0, Face f1,
                  Scalar tol,
                  SurfaceMesh::Halfedge_property<Vec3> htex,
                  SurfaceMesh::Halfedge_property<Vec3> hnorm,
                  SurfaceMesh::Face_property<int> fmat)
{
    Halfedge h0 = mesh.halfedge(e, 0);
    if (mesh.face(h0) != f0) h0 = mesh.opposite_halfedge(h0);
    Halfedge hd = mesh.opposite_halfedge(h0);

    Vertex vx = mesh.from_vertex(h0);
    Vertex vy = mesh.to_vertex(h0);

    Vertex c0(-1), c1(-1);
    for (auto v : mesh.vertices(f0)) if (v != vx && v != vy) c0 = v;
    for (auto v : mesh.vertices(f1)) if (v != vx && v != vy) c1 = v;

    // must share exactly one edge and bound four distinct vertices
    if (!c0.is_valid() || !c1.is_valid()) return false;
    if (vx == vy || vx == c0 || vx == c1 ||
        vy == c0 || vy == c1 || c0 == c1) return false;

    const Vec3 px = mesh.position(vx);
    const Vec3 py = mesh.position(vy);
    const Vec3 p0 = mesh.position(c0);
    const Vec3 p1 = mesh.position(c1);

    const Vec3 pts[4] = { px, py, p0, p1 };

    // normalize every threshold with the longest four-point distance L
    Scalar L(0);
    for (int i = 0; i < 4; ++i)
        for (int j = i+1; j < 4; ++j)
            L = std::max(L, (pts[i]-pts[j]).norm());
    if (!(L > Scalar(0))) return false;
    const Scalar L2 = L*L;
    const Scalar L3 = L2*L;

    // both triangles must have a non-degenerate area vector
    Vec3 a0 = (py-px).cross(p0-px);
    Vec3 a1 = (px-py).cross(p1-py);
    if (a0.norm() <= tol*L2 || a1.norm() <= tol*L2) return false;

    Vec3 n0 = a0.normalized();
    Vec3 n1 = a1.normalized();

    // the two faces have to face the same side of the shared edge
    if (!(n0.dot(n1) > Scalar(0))) return false;

    // coplanarity: mixed product of the four points
    Scalar triple = (py-px).dot((p0-px).cross(p1-px));
    if (std::abs(triple) > tol*L3) return false;

    // strict convexity of the outer ring, oriented as [vy, c0, vx, c1]
    Vec3 ring[4] = { py, p0, px, p1 };
    Vec3 nref = (n0+n1).normalized();
    for (int i = 0; i < 4; ++i)
    {
        Vec3 e_in  = ring[(i+1) % 4] - ring[i];
        Vec3 e_out = ring[(i+2) % 4] - ring[(i+1) % 4];
        if (e_in.cross(e_out).dot(nref) <= tol*L2) return false;
    }

    // material boundary breaks the candidate
    if (fmat && fmat[f0] != fmat[f1]) return false;

    // texture seams and hard normals break the candidate: the corner
    // values at both diagonal endpoints must match component-wise and be
    // finite. Values are never averaged or recomputed.
    if (htex)
    {
        const Vec3& t_vy0 = htex[h0];                        // f0 corner at vy
        const Vec3& t_vy1 = htex[mesh.prev_halfedge(hd)];   // f1 corner at vy
        const Vec3& t_vx0 = htex[mesh.prev_halfedge(h0)];   // f0 corner at vx
        const Vec3& t_vx1 = htex[hd];                        // f1 corner at vx
        for (int k = 0; k < 3; ++k)
        {
            if (!std::isfinite(t_vy0[k]) || !std::isfinite(t_vy1[k]) ||
                !std::isfinite(t_vx0[k]) || !std::isfinite(t_vx1[k]))
                return false;
            if (t_vy0[k] != t_vy1[k] || t_vx0[k] != t_vx1[k])
                return false;
        }
    }
    if (hnorm)
    {
        const Vec3& n_vy0 = hnorm[h0];
        const Vec3& n_vy1 = hnorm[mesh.prev_halfedge(hd)];
        const Vec3& n_vx0 = hnorm[mesh.prev_halfedge(h0)];
        const Vec3& n_vx1 = hnorm[hd];
        for (int k = 0; k < 3; ++k)
        {
            if (!std::isfinite(n_vy0[k]) || !std::isfinite(n_vy1[k]) ||
                !std::isfinite(n_vx0[k]) || !std::isfinite(n_vx1[k]))
                return false;
            if (n_vy0[k] != n_vy1[k] || n_vx0[k] != n_vx1[k])
                return false;
        }
    }

    return true;
}

} // anonymous namespace


HEADERONLY_INLINE
std::vector<SurfaceMesh::Edge>
quadrangulate(SurfaceMesh& mesh,
              const std::vector<SurfaceMesh::Face>& region,
              Scalar tolerance)
{
    // --- validate everything before touching the mesh ---------------------
    if (!std::isfinite(tolerance) || !(tolerance > Scalar(0)))
        throw std::invalid_argument("quadrangulate: tolerance must be a positive finite number");

    // an empty (or all-duplicate) selection leaves the mesh untouched
    if (region.empty())
        return std::vector<Edge>();

    std::set<int> unique_faces;
    for (Face f : region)
    {
        if (!f.is_valid() || !mesh.is_valid(f) || mesh.is_deleted(f))
            throw std::invalid_argument("quadrangulate: region contains an invalid or deleted face");
        if (mesh.valence(f) != 3)
            throw std::invalid_argument("quadrangulate: region contains a non-triangle face");
        for (auto v : mesh.vertices(f))
        {
            if (!mesh.is_valid(v) || mesh.is_deleted(v) ||
                !finite_vec(mesh.position(v)))
                throw std::invalid_argument("quadrangulate: region triangle has non-finite coordinates");
        }
        unique_faces.insert(f.idx());
    }

    const int n = (int)unique_faces.size();
    std::vector<Face> faces;
    faces.reserve(n);
    std::vector<int> node_of_face(mesh.faces_size(), -1);
    for (int idx : unique_faces)
    {
        node_of_face[idx] = (int)faces.size();
        faces.push_back(Face(idx));
    }

    SurfaceMesh::Halfedge_property<Vec3> htex = mesh.get_halfedge_property<Vec3>("h:texcoord");
    SurfaceMesh::Halfedge_property<Vec3> hnorm = mesh.get_halfedge_property<Vec3>("h:normal");
    SurfaceMesh::Face_property<int> fmat = mesh.get_face_property<int>("f:material");

    // --- build the candidate graph ----------------------------------------
    struct Candidate { int u, v; Edge edge; };
    std::vector<Candidate> candidates;
    std::vector<std::vector<int> > graph(n);

    for (int u = 0; u < n; ++u)
    {
        Face f = faces[u];
        for (auto h : mesh.halfedges(f))
        {
            Edge e = mesh.edge(h);
            Halfedge oh = mesh.opposite_halfedge(h);
            if (mesh.is_boundary(oh)) continue;

            Face g = mesh.face(oh);
            if (!g.is_valid() || mesh.is_deleted(g) || mesh.valence(g) != 3)
                continue;

            int v = node_of_face[g.idx()];
            if (v == -1 || v <= u) continue; // each pair considered once

            if (pair_is_quad(mesh, e, f, g, tolerance, htex, hnorm, fmat))
            {
                graph[u].push_back(v);
                graph[v].push_back(u);
                candidates.push_back(Candidate{u, v, e});
            }
        }
    }

    // --- maximum-cardinality matching on the general graph ----------------
    std::vector<int> match = maximum_matching(graph);

    // --- apply the (already fixed) matching with the join primitive -------
    std::vector<Edge> removed;
    std::vector<char> joined(n, 0);
    for (const Candidate& c : candidates)
    {
        if (joined[c.u] || joined[c.v]) continue;
        if (match[c.u] != c.v) continue;

        if (mesh.join_triangles(c.edge))
        {
            removed.push_back(c.edge);
            joined[c.u] = joined[c.v] = 1;
        }
    }

    std::sort(removed.begin(), removed.end(),
              [](Edge a, Edge b) { return a.idx() < b.idx(); });
    return removed;
}

//=============================================================================
} // namespace OpenGP
//=============================================================================

#endif // OPENGP_QUADRANGULATE_CPP
