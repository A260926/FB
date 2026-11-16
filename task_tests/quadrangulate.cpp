// Task tests for triangle->quad quadrangulation, join_triangles and OBJ IO.
#include "task_support/check.h"

#include <OpenGP/SurfaceMesh/SurfaceMesh.h>
#include <OpenGP/SurfaceMesh/Quadrangulate.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace OpenGP;

namespace {

typedef SurfaceMesh::Vertex   Vertex;
typedef SurfaceMesh::Halfedge Halfedge;
typedef SurfaceMesh::Edge     Edge;
typedef SurfaceMesh::Face     Face;

// --------------------------------------------------------------------------
// small mesh builders
// --------------------------------------------------------------------------

SurfaceMesh make_tri_mesh(const std::vector<Vec3>& pts,
                          const std::vector<std::array<int,3> >& tris)
{
    SurfaceMesh m;
    std::vector<Vertex> v;
    for (const Vec3& p : pts) v.push_back(m.add_vertex(p));
    for (const auto& t : tris)
    {
        Face f = m.add_triangle(v[t[0]], v[t[1]], v[t[2]]);
        (void)f;
    }
    return m;
}

// planar grid of (cols x rows) unit squares, each split into two triangles;
// diagonal direction alternates so the dual candidate graph is not trivial
SurfaceMesh make_grid_mesh(int cols, int rows, float ox = 0, float oy = 0)
{
    std::vector<Vec3> pts;
    for (int y = 0; y <= rows; ++y)
        for (int x = 0; x <= cols; ++x)
            pts.push_back(Vec3(ox + (float)x, oy + (float)y, 0));

    auto vid = [&](int x, int y) { return y * (cols + 1) + x; };

    std::vector<std::array<int,3> > tris;
    for (int y = 0; y < rows; ++y)
        for (int x = 0; x < cols; ++x)
        {
            int p00 = vid(x, y),     p10 = vid(x + 1, y);
            int p11 = vid(x + 1, y + 1), p01 = vid(x, y + 1);
            if ((x + y) % 2 == 0)
            {
                tris.push_back({p00, p10, p11});
                tris.push_back({p00, p11, p01});
            }
            else
            {
                tris.push_back({p00, p10, p01});
                tris.push_back({p10, p11, p01});
            }
        }
    return make_tri_mesh(pts, tris);
}

// --------------------------------------------------------------------------
// independent candidate graph (predicates re-derived in this test file)
// --------------------------------------------------------------------------

struct IEdge { int u, v; };

bool indep_finite(const Vec3& p)
{
    return std::isfinite(p[0]) && std::isfinite(p[1]) && std::isfinite(p[2]);
}

// independent convex-planar-quad predicate for two adjacent triangles
bool indep_pair_ok(const SurfaceMesh& m, Face f, Face g, Scalar tol)
{
    std::vector<Vertex> fv, gv;
    for (auto v : m.vertices(f)) fv.push_back(v);
    for (auto v : m.vertices(g)) gv.push_back(v);

    std::vector<Vertex> shared;
    for (Vertex a : fv)
        for (Vertex b : gv)
            if (a == b) shared.push_back(a);
    if (shared.size() != 2) return false;

    std::set<int> all;
    for (Vertex v : fv) all.insert(v.idx());
    for (Vertex v : gv) all.insert(v.idx());
    if (all.size() != 4) return false;

    std::vector<Vec3> p;
    for (int idx : all) p.push_back(m.position(Vertex(idx)));
    for (const Vec3& q : p) if (!indep_finite(q)) return false;

    Scalar L = 0;
    for (size_t i = 0; i < p.size(); ++i)
        for (size_t j = i + 1; j < p.size(); ++j)
            L = std::max(L, (p[i] - p[j]).norm());
    if (!(L > 0)) return false;

    // triangle area vectors, computed from the two triangle vertex sets
    auto area_vec = [&](const std::vector<Vertex>& tv) {
        Vec3 a = m.position(tv[0]), b = m.position(tv[1]), c = m.position(tv[2]);
        return (b - a).cross(c - a);
    };
    Vec3 av0 = area_vec(fv), av1 = area_vec(gv);
    if (av0.norm() <= tol * L * L || av1.norm() <= tol * L * L) return false;

    Vec3 n0 = av0.normalized(), n1 = av1.normalized();
    if (!(n0.dot(n1) > 0)) return false;

    // coplanarity via mixed product
    Vec3 a = m.position(fv[0]), b = m.position(fv[1]), c = m.position(fv[2]);
    Vec3 d = m.position(gv[0]);
    Vec3 d2 = (b - a).cross(c - a);
    Scalar triple = (d - a).dot(d2);
    if (std::abs(triple) > tol * L * L * L) return false;

    // strict convexity: recover the outer ring from the orientation of the
    // shared edge (f's halfedge): vy -> f-opposite -> vx -> g-opposite
    Edge shared_edge = m.find_edge(shared[0], shared[1]);
    Halfedge hf = m.halfedge(shared_edge, 0);
    if (m.face(hf) != f) hf = m.opposite_halfedge(hf);
    Vertex vy = m.to_vertex(hf);
    Vertex vx = m.from_vertex(hf);
    Vertex of(-1), og(-1);
    for (Vertex v : fv) if (v != vx && v != vy) of = v;
    for (Vertex v : gv) if (v != vx && v != vy) og = v;

    Vec3 ring[4] = { m.position(vy), m.position(of),
                     m.position(vx), m.position(og) };
    Vec3 nref = (n0 + n1).normalized();
    for (int i = 0; i < 4; ++i)
    {
        Vec3 ei = ring[(i + 1) % 4] - ring[i];
        Vec3 eo = ring[(i + 2) % 4] - ring[(i + 1) % 4];
        if (ei.cross(eo).dot(nref) <= tol * L * L) return false;
    }
    return true;
}

// build candidate graph from scratch, ignoring all attributes
std::vector<IEdge> indep_graph(const SurfaceMesh& m, const std::vector<Face>& region,
                               Scalar tol = Scalar(1e-5))
{
    std::vector<IEdge> out;
    for (size_t i = 0; i < region.size(); ++i)
        for (size_t j = i + 1; j < region.size(); ++j)
            if (indep_pair_ok(m, region[i], region[j], tol))
                out.push_back({(int)i, (int)j});
    return out;
}

// exhaustive maximum matching over all edge subsets (graphs are small)
int brute_max_matching(const std::vector<IEdge>& edges)
{
    int best = 0;
    int ne = (int)edges.size();
    for (int mask = 0; mask < (1 << ne); ++mask)
    {
        std::set<int> used;
        bool ok = true;
        int cnt = 0;
        for (int e = 0; e < ne; ++e)
            if (mask & (1 << e))
            {
                if (used.count(edges[e].u) || used.count(edges[e].v)) { ok = false; break; }
                used.insert(edges[e].u);
                used.insert(edges[e].v);
                ++cnt;
            }
        if (ok) best = std::max(best, cnt);
    }
    return best;
}

// greedy matching for one explicit edge ordering
int greedy_matching(const std::vector<IEdge>& edges, const std::vector<int>& order)
{
    std::set<int> used;
    int cnt = 0;
    for (int k : order)
    {
        if (used.count(edges[k].u) || used.count(edges[k].v)) continue;
        used.insert(edges[k].u);
        used.insert(edges[k].v);
        ++cnt;
    }
    return cnt;
}

// worst greedy result over every edge permutation (small graphs only)
int worst_greedy(const std::vector<IEdge>& edges)
{
    int ne = (int)edges.size();
    std::vector<int> order(ne);
    for (int i = 0; i < ne; ++i) order[i] = i;
    int worst = ne + 1;
    do
    {
        worst = std::min(worst, greedy_matching(edges, order));
    }
    while (std::next_permutation(order.begin(), order.end()));
    return worst;
}

std::vector<Face> all_faces(const SurfaceMesh& m)
{
    std::vector<Face> r;
    for (auto f : m.faces()) r.push_back(f);
    return r;
}

std::string tmp_path(const char* name)
{
    const char* dir = std::getenv("TMPDIR");
    std::string base = dir ? dir : "/tmp";
    return base + "/" + name;
}

int count_text_lines(const std::string& path, const char* prefix)
{
    std::ifstream in(path);
    std::string line;
    int n = 0;
    while (std::getline(in, line))
        if (line.compare(0, std::strlen(prefix), prefix) == 0) ++n;
    return n;
}

bool expect_throw(SurfaceMesh& m, const std::vector<Face>& r, Scalar tol)
{
    bool threw = false;
    try { quadrangulate(m, r, tol); }
    catch (const std::invalid_argument&) { threw = true; }
    return threw;
}

} // anonymous namespace

// ==========================================================================
// matching group
// ==========================================================================

static int matching_two_quads()
{
    // four triangles in two separate squares: optimum and result both 2
    SurfaceMesh m = make_grid_mesh(2, 1);
    std::vector<Face> region = all_faces(m);
    TASK_CHECK(region.size() == 4);

    std::vector<IEdge> g = indep_graph(m, region);
    int optimum = brute_max_matching(g);
    TASK_CHECK(optimum == 2);

    std::vector<Edge> removed = quadrangulate(m, region);
    TASK_CHECK(removed.size() == (size_t)optimum);
    TASK_CHECK(removed.size() == 2);
    TASK_CHECK(std::is_sorted(removed.begin(), removed.end(),
                              [](Edge a, Edge b) { return a.idx() < b.idx(); }));
    for (Edge e : removed) TASK_CHECK(m.is_deleted(e));

    int quads = 0;
    for (auto f : m.faces()) { TASK_CHECK(m.valence(f) == 4); ++quads; }
    TASK_CHECK(quads == 2);
    return 0;
}

static int matching_pentagon_odd_cycle()
{
    // fan triangulation of a regular pentagon: dual candidate graph is C5
    std::vector<Vec3> pts;
    pts.push_back(Vec3(0, 0, 0));
    const float pi = 3.14159265358979323846f;
    for (int i = 0; i < 5; ++i)
    {
        float a = 2 * pi * i / 5;
        pts.push_back(Vec3(std::cos(a), std::sin(a), 0));
    }
    std::vector<std::array<int,3> > tris;
    for (int i = 0; i < 5; ++i)
        tris.push_back({0, 1 + i, 1 + ((i + 1) % 5)});
    SurfaceMesh m = make_tri_mesh(pts, tris);

    std::vector<Face> region = all_faces(m);
    std::vector<IEdge> g = indep_graph(m, region);
    TASK_CHECK(g.size() == 5);                 // the five spokes -> C5
    int optimum = brute_max_matching(g);
    TASK_CHECK(optimum == 2);                  // odd cycle leaves one node free

    std::vector<Edge> removed = quadrangulate(m, region);
    TASK_CHECK(removed.size() == 2);
    int unmatched = 0;
    for (auto f : m.faces()) if (m.valence(f) == 3) ++unmatched;
    TASK_CHECK(unmatched == 1);
    return 0;
}

static int matching_greedy_insufficient()
{
    // six triangles whose candidate graph is path P6: some greedy orders
    // produce only 2 merges while the optimum has 3
    std::vector<Vec3> pts = {
        Vec3(0, 0, 0), Vec3(1, 0, 0), Vec3(2, 0, 0),
        Vec3(0, 1, 0), Vec3(1, 1, 0), Vec3(2, 1, 0),
        Vec3(0, 2, 0), Vec3(1, 2, 0)
    };
    std::vector<std::array<int,3> > tris = {
        {0, 1, 4}, {0, 4, 3},
        {1, 2, 5}, {1, 5, 4},
        {3, 4, 7}, {3, 7, 6}
    };
    SurfaceMesh m = make_tri_mesh(pts, tris);

    std::vector<Face> region = all_faces(m);
    std::vector<IEdge> g = indep_graph(m, region);
    TASK_CHECK(g.size() == 5);
    int optimum = brute_max_matching(g);
    TASK_CHECK(optimum == 3);
    TASK_CHECK(worst_greedy(g) < optimum);    // greedy can be suboptimal

    std::vector<Edge> removed = quadrangulate(m, region);
    TASK_CHECK(removed.size() == 3);          // blossom reaches the optimum
    for (auto f : m.faces()) TASK_CHECK(m.valence(f) == 4);
    return 0;
}

static int matching_odd_cycle_with_external_leaf()
{
    // real planar mesh: a heptagon fan (dual candidate graph = odd cycle C7)
    // plus one triangle attached to a boundary edge, adding a leaf node.
    // Contracting the odd cycle must still let the external edge augment,
    // otherwise the result is one merge short. Brute force gives the optimum.
    const int K = 7;
    std::vector<Vec3> pts;
    pts.push_back(Vec3(0, 0, 0));                       // center
    const float pi = 3.14159265358979323846f;
    for (int i = 0; i < K; ++i)
    {
        float a = 2 * pi * i / K;
        pts.push_back(Vec3(std::cos(a), std::sin(a), 0));
    }
    // outer vertex just beyond the ring edge between ring0 and ring1
    Vec3 mid = Scalar(0.5) * (pts[1] + pts[2]);
    Vec3 outv = mid + (mid - pts[0]).normalized() * 0.5f;
    pts.push_back(outv);                                 // index K+1

    std::vector<std::array<int,3> > tris;
    for (int i = 0; i < K; ++i)
        tris.push_back({0, 1 + i, 1 + ((i + 1) % K)});
    // attached outer triangle sharing the (ring0,ring1) edge
    tris.push_back({1, (int)pts.size() - 1, 2});

    SurfaceMesh m = make_tri_mesh(pts, tris);
    std::vector<Face> region = all_faces(m);
    TASK_CHECK(region.size() == (size_t)(K + 1));

    std::vector<IEdge> g = indep_graph(m, region);
    TASK_CHECK(g.size() == (size_t)(K + 1));            // C7 edges + one leaf
    int optimum = brute_max_matching(g);
    TASK_CHECK(optimum == 4);                            // leaf frees one more match

    std::vector<Edge> removed = quadrangulate(m, region);
    TASK_CHECK(removed.size() == 4);
    int tri_left = 0;
    for (auto f : m.faces()) if (m.valence(f) == 3) ++tri_left;
    TASK_CHECK(tri_left == 0);
    return 0;
}

static int matching_grids_vs_bruteforce()
{
    // several real planar meshes; library answer must equal the exhaustive
    // optimum of the independently reconstructed candidate graph
    const int dims[][2] = { {2, 2}, {3, 2}, {4, 3} };
    for (auto& d : dims)
    {
        SurfaceMesh m = make_grid_mesh(d[0], d[1]);
        std::vector<Face> region = all_faces(m);
        std::vector<IEdge> g = indep_graph(m, region);
        int optimum = brute_max_matching(g);

        std::vector<Edge> removed = quadrangulate(m, region);
        TASK_CHECK(removed.size() == (size_t)optimum);

        int quads = 0, tri = 0;
        for (auto f : m.faces())
        {
            if (m.valence(f) == 4) ++quads;
            else if (m.valence(f) == 3) ++tri;
        }
        TASK_CHECK((int)quads == optimum);
        TASK_CHECK(quads * 2 + tri == (int)region.size());
    }
    return 0;
}

static int matching_remote_component_and_duplicates()
{
    // two disconnected patches, one far away; both must be optimized
    SurfaceMesh m = make_grid_mesh(2, 1, 0, 0);

    auto a = m.add_vertex(Vec3(10, 10, 0));
    auto b = m.add_vertex(Vec3(11, 10, 0));
    auto c = m.add_vertex(Vec3(11, 11, 0));
    auto d = m.add_vertex(Vec3(10, 11, 0));
    auto tf0 = m.add_triangle(a, b, c);
    auto tf1 = m.add_triangle(a, c, d);

    std::vector<Face> region = all_faces(m);
    const size_t unique_count = region.size();
    region.push_back(tf0);                       // duplicate, must be ignored
    region.push_back(tf1);
    TASK_CHECK(region.size() == unique_count + 2);

    std::vector<Edge> removed = quadrangulate(m, region);
    TASK_CHECK(removed.size() == 3);             // 2 grid squares + far square
    TASK_CHECK(m.n_faces() == 3);
    for (auto f : m.faces()) TASK_CHECK(m.valence(f) == 4);
    return 0;
}

// ==========================================================================
// topology group
// ==========================================================================

static int topo_join_primitive_identity()
{
    SurfaceMesh m = make_grid_mesh(1, 1);
    Edge diag(-1);
    for (auto e : m.edges()) if (!m.is_boundary(e)) diag = e;
    TASK_CHECK(diag.is_valid());

    // snapshot the four outer halfedges and their corner attributes
    auto tex = m.halfedge_property<Vec3>("h:texcoord");
    auto nrm = m.halfedge_property<Vec3>("h:normal");
    auto mat = m.face_property<int>("f:material", 7);
    std::vector<Halfedge> outer;
    std::vector<Vec3> outer_tex, outer_nrm;
    Face f0 = m.face(m.halfedge(diag, 0));
    Face f1 = m.face(m.halfedge(diag, 1));
    Face keep = f0.idx() < f1.idx() ? f0 : f1;
    Face gone = f0.idx() < f1.idx() ? f1 : f0;
    for (auto f : std::array<Face,2>{ f0, f1 })
        for (auto h : m.halfedges(f))
            if (m.edge(h) != diag)
            {
                outer.push_back(h);
                outer_tex.push_back(tex[h] = Vec3(h.idx(), 1, 2));
                outer_nrm.push_back(nrm[h] = Vec3(0, 0, 1));
            }
    TASK_CHECK(outer.size() == 4);

    unsigned int nfaces = m.n_faces();
    TASK_CHECK(m.join_triangles(diag));
    TASK_CHECK(m.n_faces() == nfaces - 1);
    TASK_CHECK(m.is_deleted(diag));
    TASK_CHECK(m.is_deleted(gone));
    TASK_CHECK(!m.is_deleted(keep));
    TASK_CHECK(m.valence(keep) == 4);
    TASK_CHECK(mat[keep] == 7);

    // the quad must use exactly the four surviving halfedge identities
    std::set<int> seen;
    for (auto h : m.halfedges(keep)) { seen.insert(h.idx()); TASK_CHECK(m.face(h) == keep); }
    TASK_CHECK(seen.size() == 4);
    for (Halfedge h : outer) TASK_CHECK(seen.count(h.idx()) == 1);

    // attributes on outer halfedges untouched
    size_t k = 0;
    for (Halfedge h : outer)
    {
        TASK_CHECK((tex[h] - outer_tex[k]).norm() < 1e-7f);
        TASK_CHECK((nrm[h] - outer_nrm[k]).norm() < 1e-7f);
        ++k;
    }

    // circulators and normal computation work immediately
    Vec3 n = m.compute_face_normal(keep);
    TASK_CHECK(std::abs(n[2] - 1) < 1e-6f);
    int vcnt = 0;
    for (auto v : m.vertices(keep)) { (void)v; ++vcnt; }
    TASK_CHECK(vcnt == 4);

    // vertex one-rings stay consistent while deleted slots still exist:
    // each corner vertex has exactly its two quad neighbors and one face
    for (auto v : m.vertices(keep))
    {
        std::set<int> nbr;
        auto vc = m.vertices(v), ve = vc;
        if (vc) do { nbr.insert((*vc).idx()); } while (++vc != ve);
        TASK_CHECK(nbr.size() == 2);
        TASK_CHECK(m.is_manifold(v));
        TASK_CHECK(m.halfedge(v).is_valid() && !m.is_deleted(m.halfedge(v)));
        int fseen = 0;
        auto fc = m.faces(v), fe = fc;
        if (fc) do { TASK_CHECK(*fc == keep); ++fseen; } while (++fc != fe);
        TASK_CHECK(fseen == 1);
    }

    // garbage collection and triangulation work immediately afterwards
    m.garbage_collection();
    TASK_CHECK(m.n_faces() == 1);
    Face q = *m.faces_begin();
    TASK_CHECK(m.valence(q) == 4);
    m.triangulate(q);
    TASK_CHECK(m.n_faces() == 2);
    for (auto f : m.faces()) TASK_CHECK(m.valence(f) == 3);
    return 0;
}

static int topo_join_rejects_bad_topology()
{
    // boundary edge, deleted edge, edge of a quad
    SurfaceMesh m = make_grid_mesh(1, 1);
    Edge boundary(-1), interior(-1);
    for (auto e : m.edges())
    {
        if (m.is_boundary(e)) boundary = e;
        else interior = e;
    }
    TASK_CHECK(boundary.is_valid() && interior.is_valid());
    TASK_CHECK(!m.join_triangles(boundary));
    TASK_CHECK(!m.join_triangles(Edge(-1)));
    TASK_CHECK(!m.join_triangles(Edge(9999)));
    TASK_CHECK(m.n_faces() == 2);

    TASK_CHECK(m.join_triangles(interior));
    TASK_CHECK(!m.join_triangles(interior));     // now deleted
    TASK_CHECK(m.n_faces() == 1);

    // edge incident to a non-triangle face
    SurfaceMesh q;
    auto q0 = q.add_vertex(Vec3(0, 0, 0));
    auto q1 = q.add_vertex(Vec3(1, 0, 0));
    auto q2 = q.add_vertex(Vec3(1, 1, 0));
    auto q3 = q.add_vertex(Vec3(0, 1, 0));
    auto q4 = q.add_vertex(Vec3(0, 1, 1));
    auto q5 = q.add_vertex(Vec3(1, 1, 1));
    q.add_quad(q0, q1, q2, q3);
    q.add_triangle(q3, q2, q5);
    q.add_triangle(q3, q5, q4);
    Edge shared = q.find_edge(q2, q3);
    TASK_CHECK(shared.is_valid());
    TASK_CHECK(!q.join_triangles(shared));
    TASK_CHECK(q.n_faces() == 3);
    return 0;
}

static int topo_hole_and_boundary()
{
    // 3x3 cells with the center cell left open (a hole)
    const int N = 4;
    std::vector<Vec3> pts;
    for (int y = 0; y < N; ++y)
        for (int x = 0; x < N; ++x)
            pts.push_back(Vec3((float)x, (float)y, 0));
    auto vid = [&](int x, int y) { return y * N + x; };
    std::vector<std::array<int,3> > tris;
    for (int y = 0; y < 3; ++y)
        for (int x = 0; x < 3; ++x)
        {
            if (x == 1 && y == 1) continue;      // hole
            int p00 = vid(x, y), p10 = vid(x + 1, y);
            int p11 = vid(x + 1, y + 1), p01 = vid(x, y + 1);
            tris.push_back({p00, p10, p11});
            tris.push_back({p00, p11, p01});
        }
    SurfaceMesh m = make_tri_mesh(pts, tris);

    int boundary_edges_before = 0;
    for (auto e : m.edges()) if (m.is_boundary(e)) ++boundary_edges_before;
    TASK_CHECK(boundary_edges_before >= 8);      // outer rim + hole rim

    std::vector<Face> region = all_faces(m);
    std::vector<Edge> removed = quadrangulate(m, region);
    TASK_CHECK(!removed.empty());

    int boundary_edges_after = 0;
    for (auto e : m.edges()) if (m.is_boundary(e)) ++boundary_edges_after;
    TASK_CHECK(boundary_edges_before == boundary_edges_after);

    // no quad touches the hole with a removed diagonal: every remaining
    // boundary edge is still an outer edge of a face
    m.garbage_collection();
    for (auto e : m.edges())
        if (m.is_boundary(e))
        {
            Halfedge h = m.halfedge(e, 0);
            if (m.is_boundary(h)) h = m.opposite_halfedge(h);
            TASK_CHECK(m.face(h).is_valid());
        }
    return 0;
}

static int topo_material_blocks()
{
    SurfaceMesh m = make_grid_mesh(2, 1);
    auto mat = m.face_property<int>("f:material", 0);
    std::vector<Face> faces = all_faces(m);
    // split the four triangles into two materials across the middle edge
    for (Face f : faces)
    {
        bool right = false;
        for (auto v : m.vertices(f)) if (m.position(v)[0] > 1.5f) right = true;
        mat[f] = right ? 1 : 0;
    }
    std::vector<Edge> removed = quadrangulate(m, faces);
    TASK_CHECK(removed.size() == 2);            // only within each square
    int quads = 0;
    for (auto f : m.faces())
    {
        TASK_CHECK(m.valence(f) == 4);
        ++quads;
    }
    TASK_CHECK(quads == 2);

    // now make every material different: no merge is possible
    SurfaceMesh m2 = make_grid_mesh(1, 1);
    auto mat2 = m2.face_property<int>("f:material", 0);
    int k = 0;
    for (auto f : m2.faces()) mat2[f] = ++k;
    TASK_CHECK(quadrangulate(m2, all_faces(m2)).empty());
    TASK_CHECK(m2.n_faces() == 2);
    return 0;
}

static int topo_uv_and_normal_blocks()
{
    // identical corners allow the merge; a seam or hard edge blocks it
    auto build = [](bool seam_uv, bool seam_n, bool bad_value) {
        SurfaceMesh m = make_grid_mesh(1, 1);
        auto tex = m.halfedge_property<Vec3>("h:texcoord");
        auto nrm = m.halfedge_property<Vec3>("h:normal");
        for (auto h : m.halfedges())
        {
            tex[h] = Vec3(0.25f, 0.5f, 1);
            nrm[h] = Vec3(0, 0, 1);
        }
        Edge diag(-1);
        for (auto e : m.edges()) if (!m.is_boundary(e)) diag = e;
        Halfedge h0 = m.halfedge(diag, 0);
        if (seam_uv) tex[h0] = bad_value ? Vec3(std::nanf(""), 0, 0)
                                         : Vec3(0.9f, 0.5f, 1);
        if (seam_n)  nrm[h0] = bad_value ? Vec3(0, std::nanf(""), 0)
                                         : Vec3(0, 0, -1);
        return m;
    };

    {
        SurfaceMesh m = build(false, false, false);
        TASK_CHECK(quadrangulate(m, all_faces(m)).size() == 1);
    }
    {
        SurfaceMesh m = build(true, false, false);
        TASK_CHECK(quadrangulate(m, all_faces(m)).empty());
        TASK_CHECK(m.n_faces() == 2);
    }
    {
        SurfaceMesh m = build(false, true, false);
        TASK_CHECK(quadrangulate(m, all_faces(m)).empty());
    }
    {
        SurfaceMesh m = build(true, false, true);
        TASK_CHECK(quadrangulate(m, all_faces(m)).empty());
    }
    {
        SurfaceMesh m = build(false, true, true);
        TASK_CHECK(quadrangulate(m, all_faces(m)).empty());
    }
    return 0;
}

static int topo_nonplanar_nonconvex_degenerate()
{
    // folded pair: coplanarity must fail
    {
        SurfaceMesh m;
        auto a = m.add_vertex(Vec3(0, 0, 0));
        auto b = m.add_vertex(Vec3(1, 0, 0));
        auto c = m.add_vertex(Vec3(1, 1, 0));
        auto d = m.add_vertex(Vec3(0, 1, 0.2f));     // lifted corner
        m.add_triangle(a, b, c);
        m.add_triangle(a, c, d);
        TASK_CHECK(quadrangulate(m, all_faces(m)).empty());
    }
    // concave (arrow) quad in plane: the two triangles form a non-convex
    // four-point set, so the shared edge cannot be a candidate
    {
        SurfaceMesh m;
        auto a = m.add_vertex(Vec3(0, 0, 0));
        auto b = m.add_vertex(Vec3(2, 0, 0));
        auto c = m.add_vertex(Vec3(1, 0.5f, 0));     // indentation
        auto d = m.add_vertex(Vec3(2, 1, 0));
        auto e = m.add_vertex(Vec3(0, 1, 0));
        m.add_triangle(a, b, c);
        m.add_triangle(a, c, e);
        m.add_triangle(b, d, c);
        m.add_triangle(c, d, e);
        // two pairs meet at the indentation; neither is a strict convex quad
        std::vector<Edge> removed = quadrangulate(m, all_faces(m));
        TASK_CHECK(removed.empty());
        TASK_CHECK(m.n_faces() == 4);
    }
    // degenerate zero-area triangle
    {
        SurfaceMesh m;
        auto a = m.add_vertex(Vec3(0, 0, 0));
        auto b = m.add_vertex(Vec3(1, 0, 0));
        auto c = m.add_vertex(Vec3(1, 0, 0));         // duplicate point
        auto d = m.add_vertex(Vec3(0, 1, 0));
        m.add_triangle(a, b, c);
        m.add_triangle(a, c, d);
        TASK_CHECK(quadrangulate(m, all_faces(m)).empty());
    }
    return 0;
}

static int topo_deleted_slots()
{
    SurfaceMesh m = make_grid_mesh(2, 2);
    // delete one triangle and garbage collect so slots are compacted
    Face victim = *m.faces_begin();
    m.delete_face(victim);
    m.garbage_collection();
    TASK_CHECK(m.n_faces() == m.faces_size());  // no deleted slots left

    std::vector<Face> region = all_faces(m);
    TASK_CHECK(!region.empty());
    std::vector<Edge> removed = quadrangulate(m, region);
    TASK_CHECK(!removed.empty());
    for (Edge e : removed) TASK_CHECK(e.idx() >= 0 && e.idx() < (int)m.edges_size());

    // deleted face handle must be rejected before modification
    SurfaceMesh m2 = make_grid_mesh(1, 1);
    Face fdel = *m2.faces_begin();
    m2.delete_face(fdel);
    bool threw = false;
    try { quadrangulate(m2, std::vector<Face>{ fdel }); }
    catch (const std::invalid_argument&) { threw = true; }
    TASK_CHECK(threw);
    return 0;
}

static int topo_compress_triangulate_repair()
{
    SurfaceMesh m = make_grid_mesh(2, 2);
    auto tex = m.halfedge_property<Vec3>("h:texcoord");
    auto nrm = m.halfedge_property<Vec3>("h:normal");
    auto mat = m.face_property<int>("f:material", 3);
    // corner values derived from the vertex position so shared corners always
    // agree and no artificial seam blocks the geometric candidates
    for (auto h : m.halfedges())
    {
        const Vec3& p = m.position(m.to_vertex(h));
        tex[h] = Vec3(0.1f * p[0], 0.1f * p[1], 1);
        nrm[h] = Vec3(0, 0, 1);
    }

    std::vector<Face> region0 = all_faces(m);
    int optimum = brute_max_matching(indep_graph(m, region0));
    TASK_CHECK(optimum >= 1);
    std::vector<Edge> first = quadrangulate(m, region0);
    TASK_CHECK(first.size() == (size_t)optimum);
    m.garbage_collection();
    TASK_CHECK(m.n_faces() == region0.size() - optimum);
    for (auto f : m.faces()) TASK_CHECK(m.valence(f) == 4);

    // attributes of every surviving directed corner must still be present
    for (auto f : m.faces())
        for (auto h : m.halfedges(f))
        {
            TASK_CHECK(std::isfinite(tex[h][0]));
            TASK_CHECK(std::abs(nrm[h][2] - 1) < 1e-7f);
            TASK_CHECK(mat[f] == 3);
        }

    // re-triangulate with the fan rule and pair again: the fan restores the
    // original diagonal, so the candidate graph has the same optimum
    m.triangulate();
    TASK_CHECK(m.n_faces() == (unsigned)region0.size());
    for (auto f : m.faces()) TASK_CHECK(m.valence(f) == 3);

    std::vector<Face> region1 = all_faces(m);
    int optimum2 = brute_max_matching(indep_graph(m, region1));
    std::vector<Edge> second = quadrangulate(m, region1);
    TASK_CHECK(second.size() == (size_t)optimum2);
    TASK_CHECK(m.n_faces() == region0.size() - optimum2);
    return 0;
}

static int topo_triangulate_attribute_rule()
{
    SurfaceMesh m;
    auto v0 = m.add_vertex(Vec3(0, 0, 0));
    auto v1 = m.add_vertex(Vec3(1, 0, 0));
    auto v2 = m.add_vertex(Vec3(1, 1, 0));
    auto v3 = m.add_vertex(Vec3(0, 1, 0));
    Face f = m.add_quad(v0, v1, v2, v3);
    auto tex = m.halfedge_property<Vec3>("h:texcoord");
    auto nrm = m.halfedge_property<Vec3>("h:normal");
    auto mat = m.face_property<int>("f:material", 5);

    for (auto h : m.halfedges(f))
    {
        Vertex to = m.to_vertex(h);
        tex[h] = Vec3((float)to.idx(), 10, 20);
        nrm[h] = Vec3((float)to.idx(), 100, 200);
    }

    m.triangulate(f);
    TASK_CHECK(m.n_faces() == 2);
    for (auto nf : m.faces())
    {
        TASK_CHECK(mat[nf] == 5);
        for (auto h : m.halfedges(nf))
        {
            Vertex to = m.to_vertex(h);
            TASK_CHECK((tex[h] - Vec3((float)to.idx(), 10, 20)).norm() < 1e-7f);
            TASK_CHECK((nrm[h] - Vec3((float)to.idx(), 100, 200)).norm() < 1e-7f);
        }
    }
    return 0;
}

static int topo_validation_and_empty()
{
    SurfaceMesh m = make_grid_mesh(1, 1);
    auto faces = all_faces(m);

    TASK_CHECK(expect_throw(m, faces, 0.0f));
    TASK_CHECK(expect_throw(m, faces, -1e-5f));
    TASK_CHECK(expect_throw(m, faces, std::numeric_limits<Scalar>::infinity()));
    TASK_CHECK(expect_throw(m, faces, std::nanf("")));
    TASK_CHECK(expect_throw(m, std::vector<Face>{ Face(-1) }, 1e-5f));
    TASK_CHECK(expect_throw(m, std::vector<Face>{ Face(9999) }, 1e-5f));

    // non-triangle face
    SurfaceMesh q;
    auto a = q.add_vertex(Vec3(0, 0, 0)), b = q.add_vertex(Vec3(1, 0, 0));
    auto c = q.add_vertex(Vec3(1, 1, 0)), d = q.add_vertex(Vec3(0, 1, 0));
    Face qf = q.add_quad(a, b, c, d);
    TASK_CHECK(expect_throw(q, std::vector<Face>{ qf }, 1e-5f));

    // non-finite coordinate
    SurfaceMesh bad = make_grid_mesh(1, 1);
    bad.position(*bad.vertices_begin()) = Vec3(std::nanf(""), 0, 0);
    TASK_CHECK(expect_throw(bad, all_faces(bad), 1e-5f));

    // failed calls must not change the mesh
    TASK_CHECK(m.n_faces() == 2);

    // empty region changes nothing and returns nothing
    TASK_CHECK(quadrangulate(m, std::vector<Face>()).empty());
    TASK_CHECK(m.n_faces() == 2);

    // but an invalid tolerance is still rejected on an empty region
    TASK_CHECK(expect_throw(m, std::vector<Face>(), 0.0f));
    TASK_CHECK(expect_throw(m, std::vector<Face>(), std::nanf("")));
    TASK_CHECK(m.n_faces() == 2);
    return 0;
}

// ==========================================================================
// obj group
// ==========================================================================

static int obj_handwritten_independent_indices()
{
    // independent v/vt/vn counts, all four face forms and negative indices
    std::string path = tmp_path("opengp_handwritten.obj");
    {
        std::ofstream out(path);
        out <<
            "v 0 0 0\nv 1 0 0\nv 1 1 0\nv 0 1 0\n"          // 1..4
            "v 5 0 0\nv 6 0 0\nv 6 1 0\n"                  // 5..7
            "v 5 0 1\nv 6 0 1\nv 6 1 1\n"                  // 8..10
            "v 5 0 2\nv 6 0 2\nv 6 1 2\n"                  // 11..13
            "v 5 0 3\nv 6 0 3\nv 6 1 3\n";                 // 14..16
        out <<
            "vt 0.1 0.1\nvt 0.2 0.1\nvt 0.2 0.2\n"
            "vt 0.1 0.2\nvt 0.5 0.5\nvt 0.9 0.9\n";        // 6 vt
        out <<
            "vn 0 0 1\nvn 0 0 -1\nvn 1 0 0\nvn 0 1 0\n"
            "vn 0.5 0.5 0.7\nvn -0.5 0.5 0.7\n"
            "vn 0.5 -0.5 0.7\nvn -0.5 -0.5 0.7\n";         // 8 vn
        out << "f 1 2 3 4\n";                              // v only (quad)
        out << "f 5/1/1 6/2/2 7/3/3\n";                    // v/vt/vn
        out << "f 8//1 9//4 10//3\n";                      // v//vn
        out << "f 11/6 12/5 13/4\n";                       // v/vt
        out << "f -3/-3/-3 -2/-2/-2 -1/-1/-1\n";           // negative, v/vt/vn
    }

    SurfaceMesh m;
    TASK_CHECK(m.read(path));
    TASK_CHECK(m.n_vertices() == 16);
    TASK_CHECK(m.n_faces() == 5);

    auto tex = m.get_halfedge_property<Vec3>("h:texcoord");
    auto nrm = m.get_halfedge_property<Vec3>("h:normal");
    TASK_CHECK(bool(tex));
    TASK_CHECK(bool(nrm));

    // expected corner values, resolved with the same index rules
    std::vector<Vec3> vts = {
        Vec3(0.1f,0.1f,1), Vec3(0.2f,0.1f,1), Vec3(0.2f,0.2f,1),
        Vec3(0.1f,0.2f,1), Vec3(0.5f,0.5f,1), Vec3(0.9f,0.9f,1)
    };
    std::vector<Vec3> vns = {
        Vec3(0,0,1), Vec3(0,0,-1), Vec3(1,0,0), Vec3(0,1,0),
        Vec3(0.5f,0.5f,0.7f), Vec3(-0.5f,0.5f,0.7f),
        Vec3(0.5f,-0.5f,0.7f), Vec3(-0.5f,-0.5f,0.7f)
    };
    auto resolve = [](int idx, int n) { return idx > 0 ? idx - 1 : n + idx; };

    struct Expect { int face; int corner; Vec3 t; Vec3 n; bool has_t; bool has_n; };
    std::vector<Expect> expects;
    // face 2: v/vt/vn, corners vt 1,2,3 vn 1,2,3
    for (int k = 0; k < 3; ++k)
        expects.push_back({1, k, vts[k], vns[k], true, true});
    // face 3: v//vn, vn 1,4,3
    {
        int vn_idx[3] = {0, 3, 2};
        for (int k = 0; k < 3; ++k)
            expects.push_back({2, k, Vec3(), vns[vn_idx[k]], false, true});
    }
    // face 4: v/vt, vt 6,5,4
    {
        int vt_idx[3] = {5, 4, 3};
        for (int k = 0; k < 3; ++k)
            expects.push_back({3, k, vts[vt_idx[k]], Vec3(), true, false});
    }
    // face 5: negative indices -3,-2,-1 against 16 v / 6 vt / 8 vn
    for (int k = 0; k < 3; ++k)
    {
        int neg = -(3 - k);
        int vi = resolve(neg, 16), ti = resolve(neg, 6), ni = resolve(neg, 8);
        expects.push_back({4, k, vts[ti], vns[ni], true, true});
        (void)vi;
    }

    std::vector<Face> faces = all_faces(m);
    for (const Expect& e : expects)
    {
        auto hit = m.halfedges(faces[e.face]);
        for (int k = 0; k < e.corner; ++k) ++hit;
        if (e.has_t) TASK_CHECK((tex[*hit] - e.t).norm() < 1e-6f);
        if (e.has_n) TASK_CHECK((nrm[*hit] - e.n).norm() < 1e-6f);
    }
    return 0;
}

static int obj_corner_normal_roundtrip()
{
    // hard normals: every directed corner gets its own normal
    SurfaceMesh m = make_grid_mesh(1, 1);
    auto nrm = m.halfedge_property<Vec3>("h:normal");
    auto tex = m.halfedge_property<Vec3>("h:texcoord");
    std::vector<Vec3> corner_before;
    std::vector<Vec3> uv_before;
    std::vector<Vec3> pos_before;
    int k = 0;
    for (auto f : m.faces())
        for (auto h : m.halfedges(f))
        {
            nrm[h] = Vec3(0.1f * (k % 3), 0.2f * k, 1).normalized();
            tex[h] = Vec3(0.01f * k, 0.02f * k, 1);
            corner_before.push_back(nrm[h]);
            uv_before.push_back(tex[h]);
            pos_before.push_back(m.position(m.to_vertex(h)));
            ++k;
        }
    TASK_CHECK(k == 6);

    std::string path = tmp_path("opengp_corner_normals.obj");
    TASK_CHECK(m.write(path));
    TASK_CHECK(count_text_lines(path, "vn") >= 6);

    // on-disk statement order must be v, then vt, then vn, then faces
    {
        std::ifstream in(path);
        std::string line;
        int stage = 0; // 0:v 1:vt 2:vn 3:f
        bool ordered = true;
        while (std::getline(in, line))
        {
            int s;
            if (line.compare(0, 2, "v ") == 0)       s = 0;
            else if (line.compare(0, 3, "vt ") == 0) s = 1;
            else if (line.compare(0, 3, "vn ") == 0) s = 2;
            else if (line.compare(0, 2, "f ") == 0)  s = 3;
            else continue;
            if (s < stage) ordered = false;
            stage = std::max(stage, s);
        }
        TASK_CHECK(ordered);
    }

    SurfaceMesh r;
    TASK_CHECK(r.read(path));
    auto rn = r.get_halfedge_property<Vec3>("h:normal");
    auto rt = r.get_halfedge_property<Vec3>("h:texcoord");
    TASK_CHECK(bool(rn) && bool(rt));

    int i = 0;
    for (auto f : r.faces())
        for (auto h : r.halfedges(f))
        {
            TASK_CHECK((rn[h] - corner_before[i]).norm() < 1e-5f);
            TASK_CHECK((rt[h] - uv_before[i]).norm() < 1e-5f);
            TASK_CHECK((r.position(r.to_vertex(h)) - pos_before[i]).norm() < 1e-6f);
            ++i;
        }
    TASK_CHECK(i == 6);
    return 0;
}

static int obj_vertex_normal_fallback_and_no_normals()
{
    // vertex normals keep the old export path but read back as corner values
    SurfaceMesh m = make_grid_mesh(1, 1);
    auto vn = m.vertex_property<Vec3>("v:normal");
    for (auto v : m.vertices()) vn[v] = Vec3(0, 0, 1);
    std::string path = tmp_path("opengp_vnormal.obj");
    TASK_CHECK(m.write(path));
    TASK_CHECK(count_text_lines(path, "vn") == (int)m.n_vertices());

    SurfaceMesh r;
    TASK_CHECK(r.read(path));
    auto rn = r.get_halfedge_property<Vec3>("h:normal");
    TASK_CHECK(bool(rn));
    for (auto f : r.faces())
        for (auto h : r.halfedges(f))
            TASK_CHECK((rn[h] - Vec3(0, 0, 1)).norm() < 1e-6f);

    // no normals at all: no vn lines and no dangling "//" references
    SurfaceMesh plain = make_grid_mesh(1, 1);
    std::string path2 = tmp_path("opengp_plain.obj");
    TASK_CHECK(plain.write(path2));
    TASK_CHECK(count_text_lines(path2, "vn") == 0);
    std::ifstream in(path2);
    std::string line;
    while (std::getline(in, line))
        if (line.compare(0, 2, "f ") == 0)
            TASK_CHECK(line.find("//") == std::string::npos &&
                       line.find('/') == std::string::npos);
    return 0;
}

static int obj_renumber_after_gc()
{
    SurfaceMesh m = make_grid_mesh(2, 1);
    auto extra = m.add_vertex(Vec3(99, 99, 99));
    Face victim = *m.faces_begin();
    m.delete_face(victim);
    m.delete_vertex(extra);
    m.garbage_collection();

    std::vector<Vec3> live_pos;
    for (auto v : m.vertices()) live_pos.push_back(m.position(v));

    std::string path = tmp_path("opengp_renumber.obj");
    TASK_CHECK(m.write(path));

    // face indices must form a contiguous 1..n range
    std::ifstream in(path);
    std::string line;
    int max_idx = 0, v_lines = 0;
    while (std::getline(in, line))
    {
        if (line.compare(0, 2, "v ") == 0) ++v_lines;
        if (line.compare(0, 2, "f ") == 0)
        {
            std::istringstream iss(line);
            std::string tok;
            iss >> tok;
            while (iss >> tok)
            {
                int vi = std::atoi(tok.c_str());
                TASK_CHECK(vi >= 1);
                max_idx = std::max(max_idx, vi);
            }
        }
    }
    TASK_CHECK(v_lines == (int)m.n_vertices());
    TASK_CHECK(max_idx == (int)m.n_vertices());

    SurfaceMesh r;
    TASK_CHECK(r.read(path));
    TASK_CHECK(r.n_vertices() == m.n_vertices());
    size_t k = 0;
    for (auto v : r.vertices())
    {
        TASK_CHECK((r.position(v) - live_pos[k]).norm() < 1e-6f);
        ++k;
    }
    return 0;
}

static int obj_boundary_halfedges_not_corners()
{
    // one square split into two triangles: 6 face corners, 10 halfedges
    SurfaceMesh m = make_grid_mesh(1, 1);
    auto tex = m.halfedge_property<Vec3>("h:texcoord");
    int k = 0;
    for (auto f : m.faces())
        for (auto h : m.halfedges(f))
            tex[h] = Vec3(0.1f * (k++), 0, 1);
    TASK_CHECK(k == 6);
    TASK_CHECK(m.halfedges_size() == 10);

    std::string path = tmp_path("opengp_boundary.obj");
    TASK_CHECK(m.write(path));
    TASK_CHECK(count_text_lines(path, "vt") == 6);
    TASK_CHECK(count_text_lines(path, "v ") == 4);

    SurfaceMesh r;
    TASK_CHECK(r.read(path));
    TASK_CHECK(r.n_faces() == 2 && r.is_triangle_mesh());
    return 0;
}

static int obj_export_skips_garbage_and_gc_equivalence()
{
    // a mesh with per-corner normals+UVs; delete one face and export both
    // before and after garbage collection. Both OBJ files must round-trip to
    // the same directed corners and contain only live elements.
    SurfaceMesh m = make_grid_mesh(2, 1);
    auto tex = m.halfedge_property<Vec3>("h:texcoord");
    auto nrm = m.halfedge_property<Vec3>("h:normal");
    for (auto h : m.halfedges())
    {
        const Vec3& p = m.position(m.to_vertex(h));
        tex[h] = Vec3(0.1f * p[0], 0.1f * p[1], 1);
        nrm[h] = Vec3(0, 0, 1);
    }
    Face victim = *m.faces_begin();
    m.delete_face(victim);

    // order-independent set of directed faces: each face is its ordered
    // ring of (position, texcoord, normal) corner tuples
    typedef std::vector<std::array<float,9> > DirectedFace;
    auto directed_faces = [&](const SurfaceMesh& mm) {
        auto t = mm.get_halfedge_property<Vec3>("h:texcoord");
        auto n = mm.get_halfedge_property<Vec3>("h:normal");
        std::set<DirectedFace> s;
        for (auto f : mm.faces())
        {
            DirectedFace df;
            for (auto h : mm.halfedges(f))
            {
                const Vec3& p = mm.position(mm.to_vertex(h));
                df.push_back({p[0], p[1], p[2], t[h][0], t[h][1], t[h][2],
                              n[h][0], n[h][1], n[h][2]});
            }
            s.insert(df);
        }
        return s;
    };

    // export with deleted slots still present
    std::string p1 = tmp_path("opengp_garbage.obj");
    TASK_CHECK(m.write(p1));
    TASK_CHECK(count_text_lines(p1, "v ") == (int)m.n_vertices());
    SurfaceMesh r1;
    TASK_CHECK(r1.read(p1));
    TASK_CHECK(r1.n_faces() == m.n_faces());
    TASK_CHECK(directed_faces(m) == directed_faces(r1));

    // compact and export again: directed corners must match regardless of
    // the slot reordering performed by garbage collection
    m.garbage_collection();
    std::string p2 = tmp_path("opengp_compacted.obj");
    TASK_CHECK(m.write(p2));
    SurfaceMesh r2;
    TASK_CHECK(r2.read(p2));
    TASK_CHECK(directed_faces(m) == directed_faces(r2));
    // the two exports describe the same geometry, independent of slot order
    TASK_CHECK(directed_faces(r1) == directed_faces(r2));
    return 0;
}

// ==========================================================================
// registration
// ==========================================================================

const struct task_case task_cases[] = {
    {"matching", "two_quads",                 matching_two_quads},
    {"matching", "pentagon_odd_cycle",        matching_pentagon_odd_cycle},
    {"matching", "greedy_insufficient",       matching_greedy_insufficient},
    {"matching", "odd_cycle_external_leaf",   matching_odd_cycle_with_external_leaf},
    {"matching", "grids_vs_bruteforce",       matching_grids_vs_bruteforce},
    {"matching", "remote_component_duplicates", matching_remote_component_and_duplicates},

    {"topology", "join_primitive_identity",   topo_join_primitive_identity},
    {"topology", "join_rejects_bad_topology", topo_join_rejects_bad_topology},
    {"topology", "hole_and_boundary",         topo_hole_and_boundary},
    {"topology", "material_blocks",           topo_material_blocks},
    {"topology", "uv_and_normal_blocks",      topo_uv_and_normal_blocks},
    {"topology", "nonplanar_nonconvex_degenerate", topo_nonplanar_nonconvex_degenerate},
    {"topology", "deleted_slots",             topo_deleted_slots},
    {"topology", "compress_triangulate_repair", topo_compress_triangulate_repair},
    {"topology", "triangulate_attribute_rule", topo_triangulate_attribute_rule},
    {"topology", "validation_and_empty",      topo_validation_and_empty},

    {"obj", "handwritten_independent_indices", obj_handwritten_independent_indices},
    {"obj", "corner_normal_roundtrip",        obj_corner_normal_roundtrip},
    {"obj", "vertex_normal_fallback_no_normals", obj_vertex_normal_fallback_and_no_normals},
    {"obj", "renumber_after_gc",              obj_renumber_after_gc},
    {"obj", "boundary_halfedges_not_corners", obj_boundary_halfedges_not_corners},
    {"obj", "garbage_and_gc_equivalence",     obj_export_skips_garbage_and_gc_equivalence},
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
