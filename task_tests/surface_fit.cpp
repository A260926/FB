// Task regression tests for the double precision surface hierarchy and the
// HMesh::SurfaceFitSession. Geometric references in this file come from an
// independent exhaustive triangle kernel written here - never from the
// production closest_point query under test.

#include "task_support/check.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <functional>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

#include <GEL/Geometry/AABox.h>
#include <GEL/Geometry/BoundingTree.h>
#include <GEL/HMesh/HMesh.h>
#include <GEL/HMesh/SurfaceFit.h>

#define TASK_CHECK_THROW_BEGIN do { bool task_threw = false; try {
#define TASK_CHECK_THROW_END(exc) } catch(const exc&) { task_threw = true; } \
    catch(...) {} TASK_CHECK(task_threw); } while(0)

using namespace CGLA;
using namespace HMesh;
using Geometry::AABox;
using Geometry::BoundingTree;
using Geometry::ClosestPointResult;
using Geometry::SurfaceTriangle;

namespace
{
    // ---------------------------------------------------------------
    // Independent exhaustive geometry, kept separate from production code.
    // ---------------------------------------------------------------
    struct TriD
    {
        uint64_t id;
        Vec3d a, b, c;
    };

    struct BruteHit
    {
        uint64_t id = 0;
        Vec3d point;
        std::array<double, 3> bary{};
        double sq_dist = 0.0;
    };

    Vec3d clamp_edge(const Vec3d& p, const Vec3d& a, const Vec3d& b, double& t)
    {
        const Vec3d ab = b-a;
        t = dot(p-a, ab)/sqr_length(ab);
        t = std::clamp(t, 0.0, 1.0);
        return a + t*ab;
    }

    // Exhaustive projection: plane interior plus the three edges and three
    // vertices; the best candidate wins. Ties go to the smaller source id.
    BruteHit brute_closest(const std::vector<TriD>& tris, const Vec3d& p)
    {
        BruteHit best;
        best.sq_dist = std::numeric_limits<double>::infinity();
        for(const TriD& t: tris)
        {
            Vec3d cand;
            std::array<double,3> w{};
            const Vec3d ab = t.b-t.a, ac = t.c-t.a, ap = p-t.a;
            const double d00 = dot(ab,ab), d01 = dot(ab,ac), d11 = dot(ac,ac);
            const double d20 = dot(ap,ab), d21 = dot(ap,ac);
            const double denom = d00*d11-d01*d01;
            double v = (d11*d20-d01*d21)/denom;
            double wgt = (d00*d21-d01*d20)/denom;
            if(v >= 0.0 && wgt >= 0.0 && v+wgt <= 1.0)
            {
                w = {1.0-v-wgt, v, wgt};
                cand = t.a + ab*v + ac*wgt;
            }
            else
            {
                double best_e = std::numeric_limits<double>::infinity();
                Vec3d q;
                double tt;
                Vec3d e = clamp_edge(p, t.a, t.b, tt);
                double de = sqr_length(p-e);
                if(de < best_e) { best_e = de; q = e; }
                e = clamp_edge(p, t.b, t.c, tt);
                de = sqr_length(p-e);
                if(de < best_e) { best_e = de; q = e; }
                e = clamp_edge(p, t.c, t.a, tt);
                de = sqr_length(p-e);
                if(de < best_e) { best_e = de; q = e; }
                const Vec3d verts[3] = {t.a, t.b, t.c};
                for(const Vec3d& vx: verts)
                {
                    de = sqr_length(p-vx);
                    if(de < best_e) { best_e = de; q = vx; }
                }
                cand = q;
                // Barycentric weights of the clamped candidate by solving a
                // small 2x2 system (fallback to nearest vertex weights).
                double l00 = dot(ab,ab), l01 = dot(ab,ac), l11 = dot(ac,ac);
                const Vec3d aq = cand-t.a;
                const double det = l00*l11-l01*l01;
                double bb = (l11*dot(aq,ab)-l01*dot(aq,ac))/det;
                double cc = (l00*dot(aq,ac)-l01*dot(aq,ab))/det;
                bb = std::clamp(bb, 0.0, 1.0);
                cc = std::clamp(cc, 0.0, 1.0);
                if(bb+cc > 1.0)
                {
                    const double s = 1.0/(bb+cc);
                    bb *= s; cc *= s;
                }
                w = {1.0-bb-cc, bb, cc};
            }
            const double sd = sqr_length(p-cand);
            if(sd < best.sq_dist || (sd == best.sq_dist && t.id < best.id))
            {
                best.sq_dist = sd;
                best.id = t.id;
                best.point = cand;
                best.bary = w;
            }
        }
        return best;
    }

    std::vector<TriD> make_triangles(const std::vector<SurfaceTriangle>& in)
    {
        std::vector<TriD> out;
        out.reserve(in.size());
        for(const auto& t: in) out.push_back({t.source_id, t.v0, t.v1, t.v2});
        return out;
    }

    bool close(double a, double b, double tol = 1e-10)
    {
        return std::fabs(a-b) <= tol*(1.0+std::max(std::fabs(a), std::fabs(b)));
    }

    bool vec_close(const Vec3d& a, const Vec3d& b, double tol = 1e-10)
    {
        return close(a[0],b[0],tol) && close(a[1],b[1],tol) && close(a[2],b[2],tol);
    }

    // ---------------------------------------------------------------
    // Mesh helpers.
    // ---------------------------------------------------------------
    void build_grid(Manifold& m, int n, const std::function<Vec3d(int,int)>& point_at)
    {
        std::vector<Vec3d> points;
        std::vector<size_t> faces;
        for(int j=0;j<n;++j)
            for(int i=0;i<n;++i)
                points.push_back(point_at(i,j));
        auto idx = [n](int i, int j) { return static_cast<size_t>(j*n+i); };
        for(int j=0;j+1<n;++j)
            for(int i=0;i+1<n;++i)
            {
                const size_t a=idx(i,j), b=idx(i+1,j), c=idx(i+1,j+1), e=idx(i,j+1);
                faces.insert(faces.end(), {a,b,c,a,c,e});
            }
        build_manifold(m, points, faces, 3);
    }

    std::vector<TriD> snapshot_triangles(const Manifold& m)
    {
        std::vector<TriD> out;
        for(auto f: m.faces())
        {
            Vec3d v[3];
            int k = 0;
            for(auto vid: m.incident_vertices(f)) v[k++] = m.pos(vid);
            out.push_back({f.get_index()+1, v[0], v[1], v[2]});
        }
        return out;
    }

    std::vector<Vec3d> snapshot_positions(const Manifold& m)
    {
        std::vector<Vec3d> p;
        for(auto v: m.vertices()) p.push_back(m.pos(v));
        return p;
    }

    // Dense Gauss elimination with partial pivoting (test-only reference).
    std::vector<double> dense_solve(std::vector<std::vector<double>> A,
                                    std::vector<double> b)
    {
        const int n = static_cast<int>(b.size());
        for(int col=0;col<n;++col)
        {
            int piv = col;
            for(int r=col+1;r<n;++r)
                if(std::fabs(A[r][col]) > std::fabs(A[piv][col])) piv = r;
            std::swap(A[piv], A[col]);
            std::swap(b[piv], b[col]);
            const double d = A[col][col];
            if(!(std::fabs(d) > 0.0)) throw std::runtime_error("singular reference system");
            for(int r=0;r<n;++r)
            {
                if(r == col) continue;
                const double f = A[r][col]/d;
                for(int c=col;c<n;++c) A[r][c] -= f*A[col][c];
                b[r] -= f*b[col];
            }
        }
        std::vector<double> x(n);
        for(int i=0;i<n;++i) x[i] = b[i]/A[i][i];
        return x;
    }

    // ---------------------------------------------------------------
    // Witness group: closest point hierarchy.
    // ---------------------------------------------------------------
    int witness_face()
    {
        std::vector<SurfaceTriangle> tris;
        tris.emplace_back(7, Vec3d(0,0,0), Vec3d(4,0,0), Vec3d(0,4,0));
        BoundingTree<AABox> tree;
        tree.build_surface(tris);
        const Vec3d q(1.0, 1.0, 0.7);
        ClosestPointResult hit;
        TASK_CHECK(tree.closest_point(q, hit));
        auto ref = brute_closest(make_triangles(tris), q);
        TASK_CHECK(hit.source_id == 7);
        TASK_CHECK(close(hit.sq_dist, ref.sq_dist, 1e-14));
        TASK_CHECK(close(hit.sq_dist, 0.49, 1e-12));
        TASK_CHECK(vec_close(hit.point, Vec3d(1,1,0)));
        TASK_CHECK(close(hit.barycentric[0], 0.5));
        TASK_CHECK(close(hit.barycentric[1], 0.25));
        TASK_CHECK(close(hit.barycentric[2], 0.25));
        TASK_CHECK(close(hit.barycentric[0]+hit.barycentric[1]+hit.barycentric[2], 1.0));
        const Vec3d reconstructed = hit.barycentric[0]*tris[0].v0
                                  + hit.barycentric[1]*tris[0].v1
                                  + hit.barycentric[2]*tris[0].v2;
        TASK_CHECK(vec_close(reconstructed, hit.point));
        TASK_CHECK(hit.node_visits > 0 && hit.triangle_tests == 1);
        return 0;
    }

    int witness_edge()
    {
        std::vector<SurfaceTriangle> tris;
        tris.emplace_back(3, Vec3d(0,0,0), Vec3d(2,0,0), Vec3d(0,2,0));
        BoundingTree<AABox> tree;
        tree.build_surface(tris);
        const Vec3d q(2.0, 2.0, 0.5); // outside the hypotenuse, edge region
        ClosestPointResult hit;
        TASK_CHECK(tree.closest_point(q, hit));
        auto ref = brute_closest(make_triangles(tris), q);
        TASK_CHECK(hit.source_id == ref.id);
        TASK_CHECK(close(hit.sq_dist, ref.sq_dist, 1e-14));
        TASK_CHECK(vec_close(hit.point, ref.point, 1e-12));
        TASK_CHECK(close(hit.barycentric[0]+hit.barycentric[1]+hit.barycentric[2], 1.0));
        return 0;
    }

    int witness_vertex()
    {
        std::vector<SurfaceTriangle> tris;
        tris.emplace_back(11, Vec3d(0,0,0), Vec3d(1,0,0), Vec3d(0,1,0));
        BoundingTree<AABox> tree;
        tree.build_surface(tris);
        const Vec3d q(-1.0, -1.0, 0.3);
        ClosestPointResult hit;
        TASK_CHECK(tree.closest_point(q, hit));
        auto ref = brute_closest(make_triangles(tris), q);
        TASK_CHECK(hit.source_id == 11);
        TASK_CHECK(close(hit.sq_dist, ref.sq_dist, 1e-14));
        TASK_CHECK(vec_close(hit.point, Vec3d(0,0,0)));
        TASK_CHECK(close(hit.barycentric[0], 1.0));
        TASK_CHECK(close(hit.barycentric[1], 0.0));
        TASK_CHECK(close(hit.barycentric[2], 0.0));
        return 0;
    }

    int witness_tie()
    {
        // Two parallel triangles exactly one unit above/below the query:
        // squared distances are bit-identical, the smaller id must win.
        std::vector<SurfaceTriangle> tris;
        tris.emplace_back(42, Vec3d(-2,-2,0), Vec3d(2,-2,0), Vec3d(-2,2,0));
        tris.emplace_back(9,  Vec3d(-2,-2,2), Vec3d(2,-2,2), Vec3d(-2,2,2));
        BoundingTree<AABox> tree;
        tree.build_surface(tris);
        ClosestPointResult hit;
        TASK_CHECK(tree.closest_point(Vec3d(0,0,1), hit));
        TASK_CHECK(hit.source_id == 9);
        TASK_CHECK(close(hit.sq_dist, 1.0));
        TASK_CHECK(hit.triangle_tests == 2);
        return 0;
    }

    int witness_sparse_slots()
    {
        // Sparse, non contiguous source ids including a very large one.
        std::vector<SurfaceTriangle> tris;
        tris.emplace_back(5,       Vec3d(0,0,0), Vec3d(1,0,0), Vec3d(0,1,0));
        tris.emplace_back(17,      Vec3d(3,0,0), Vec3d(4,0,0), Vec3d(3,1,0));
        tris.emplace_back(1ULL<<40, Vec3d(6,0,0), Vec3d(7,0,0), Vec3d(6,1,0));
        BoundingTree<AABox> tree;
        tree.build_surface(tris);
        auto ref_tris = make_triangles(tris);
        const Vec3d queries[3] = {Vec3d(0.2,0.2,0.4), Vec3d(3.7,0.2,0.2),
                                  Vec3d(6.1,0.9,0.1)};
        for(const Vec3d& q: queries)
        {
            ClosestPointResult hit;
            TASK_CHECK(tree.closest_point(q, hit));
            auto ref = brute_closest(ref_tris, q);
            TASK_CHECK(hit.source_id == ref.id);
            TASK_CHECK(close(hit.sq_dist, ref.sq_dist, 1e-14));
            TASK_CHECK(vec_close(hit.point, ref.point, 1e-12));
        }
        return 0;
    }

    int witness_double_bias()
    {
        // Triangles only 1e-12 apart: float boxes merge, but the double
        // leaves still resolve which one is closer and at what distance.
        std::vector<SurfaceTriangle> tris;
        tris.emplace_back(1, Vec3d(-5,-5,0.0), Vec3d(5,-5,0.0), Vec3d(-5,5,0.0));
        tris.emplace_back(2, Vec3d(-5,-5,1e-12), Vec3d(5,-5,1e-12), Vec3d(-5,5,1e-12));
        BoundingTree<AABox> tree;
        tree.build_surface(tris);
        const Vec3d q(0.3, -0.7, 4e-13);
        ClosestPointResult hit;
        TASK_CHECK(tree.closest_point(q, hit));
        auto ref = brute_closest(make_triangles(tris), q);
        TASK_CHECK(hit.source_id == ref.id);
        TASK_CHECK(hit.source_id == 1);
        TASK_CHECK(close(hit.sq_dist, ref.sq_dist, 1e-18));
        TASK_CHECK(hit.sq_dist < 2.5e-25);
        return 0;
    }

    int witness_empty_preserves_witness()
    {
        BoundingTree<AABox> tree;
        TASK_CHECK(tree.empty());
        ClosestPointResult sentinel;
        sentinel.source_id = 123456;
        sentinel.sq_dist = -7.0;
        sentinel.node_visits = 99;
        const ClosestPointResult copy = sentinel;
        TASK_CHECK(!tree.closest_point(Vec3d(1,2,3), sentinel));
        TASK_CHECK(sentinel.source_id == copy.source_id);
        TASK_CHECK(sentinel.sq_dist == copy.sq_dist);
        TASK_CHECK(sentinel.node_visits == copy.node_visits);
        return 0;
    }

    int witness_invalid_arguments()
    {
        BoundingTree<AABox> tree;
        ClosestPointResult hit;
        TASK_CHECK_THROW_BEGIN
        tree.closest_point(Vec3d(std::nan(""), 0, 0), hit);
        TASK_CHECK_THROW_END(std::invalid_argument);

        auto expect_build_fail = [&](std::vector<SurfaceTriangle> bad) {
            bool threw = false;
            try { tree.build_surface(std::move(bad)); }
            catch(const std::invalid_argument&) { threw = true; }
            TASK_CHECK(threw);
        };
        std::vector<SurfaceTriangle> zero_id;
        zero_id.emplace_back(0, Vec3d(0,0,0), Vec3d(1,0,0), Vec3d(0,1,0));
        expect_build_fail(zero_id);

        std::vector<SurfaceTriangle> dup;
        dup.emplace_back(4, Vec3d(0,0,0), Vec3d(1,0,0), Vec3d(0,1,0));
        dup.emplace_back(4, Vec3d(2,0,0), Vec3d(3,0,0), Vec3d(2,1,0));
        expect_build_fail(dup);

        std::vector<SurfaceTriangle> degen;
        degen.emplace_back(6, Vec3d(0,0,0), Vec3d(1,0,0), Vec3d(2,0,0));
        expect_build_fail(degen);

        std::vector<SurfaceTriangle> nan_vert;
        nan_vert.emplace_back(6, Vec3d(0,0,0), Vec3d(1,0,0),
                              Vec3d(std::nan(""),1,0));
        expect_build_fail(nan_vert);

        std::vector<SurfaceTriangle> empty;
        expect_build_fail(empty);
        return 0;
    }

    int witness_failed_build_keeps_old_tree()
    {
        std::vector<SurfaceTriangle> tris;
        tris.emplace_back(8, Vec3d(0,0,0), Vec3d(1,0,0), Vec3d(0,1,0));
        BoundingTree<AABox> tree;
        tree.build_surface(tris);

        std::vector<SurfaceTriangle> bad;
        bad.emplace_back(0, Vec3d(0,0,0), Vec3d(1,0,0), Vec3d(0,1,0));
        bool threw = false;
        try { tree.build_surface(std::move(bad)); }
        catch(const std::invalid_argument&) { threw = true; }
        TASK_CHECK(threw);
        TASK_CHECK(!tree.empty());

        ClosestPointResult hit;
        TASK_CHECK(tree.closest_point(Vec3d(0.1,0.1,0.5), hit));
        TASK_CHECK(hit.source_id == 8);
        TASK_CHECK(close(hit.sq_dist, 0.25));
        return 0;
    }

    int witness_pruning()
    {
        std::vector<SurfaceTriangle> tris;
        // One near triangle carrying the answer.
        tris.emplace_back(1, Vec3d(-0.5,-0.5,0), Vec3d(0.5,-0.5,0), Vec3d(-0.5,0.5,0));
        // 4096 far, well separated triangles.
        for(int k=0;k<4096;++k)
        {
            const double x = 100.0 + (k%64)*1.5;
            const double y = 100.0 + (k/64)*1.5;
            tris.emplace_back(static_cast<uint64_t>(k+2),
                              Vec3d(x,y,50.0), Vec3d(x+1,y,50.0), Vec3d(x,y+1,50.0));
        }
        BoundingTree<AABox> tree;
        tree.build_surface(tris);
        ClosestPointResult hit;
        TASK_CHECK(tree.closest_point(Vec3d(-0.1,-0.1,0.2), hit));
        TASK_CHECK(hit.source_id == 1);
        TASK_CHECK(close(hit.sq_dist, 0.04));
        TASK_CHECK(hit.triangle_tests > 0);
        TASK_CHECK(hit.triangle_tests < tris.size()/8);
        TASK_CHECK(hit.node_visits >= hit.triangle_tests);
        return 0;
    }

    // ---------------------------------------------------------------
    // System group: frozen solve, energy and accepted step length.
    // ---------------------------------------------------------------
    struct FitFixture
    {
        Manifold source;
        Manifold target;
        std::vector<TriD> target_tris;
        std::vector<VertexID> vids;
        std::vector<int> fixed;
        std::vector<std::array<int,3>> faces;
        double lambda = 0.01, mu = 1e-4, d = 10.0;

        void build(int n, double shift)
        {
            build_grid(source, n, [n](int i, int j) {
                const double x = -1.0+2.0*i/(n-1);
                const double y = -1.0+2.0*j/(n-1);
                return Vec3d(x, y, 0.0);
            });
            build_grid(target, n, [n, shift](int i, int j) {
                const double x = -1.0+2.0*i/(n-1);
                const double y = -1.0+2.0*j/(n-1);
                return Vec3d(x, y, shift + 0.05*std::sin(2.0*x));
            });
            target_tris = snapshot_triangles(target);
            vids.assign(source.vertices_begin(), source.vertices_end());
            for(auto v: source.vertices())
                if(source.boundary(v)) fixed.push_back(v.get_index());
            for(auto f: source.faces())
            {
                std::array<int,3> fv{};
                int k=0;
                for(auto vid: source.incident_vertices(f)) fv[k++] = vid.get_index();
                faces.push_back(fv);
            }
        }
    };

    // Independently assemble and solve the frozen dense system at the
    // current source positions.
    std::vector<Vec3d> reference_frozen_solve(
        const FitFixture& fx, const std::vector<Vec3d>& x,
        std::size_t& active_out)
    {
        const int n = static_cast<int>(x.size());
        std::vector<char> is_fixed(n, false);
        for(int idx: fx.fixed) is_fixed[idx] = true;
        std::vector<int> free_map(n, -1);
        std::vector<int> free_v;
        for(int i=0;i<n;++i) if(!is_fixed[i]) { free_map[i] = (int)free_v.size(); free_v.push_back(i); }
        const int nf = (int)free_v.size();

        std::vector<std::vector<int>> nbr(n);
        for(auto f: fx.faces)
            for(int a=0;a<3;++a) for(int b=0;b<3;++b)
                if(a != b) nbr[f[a]].push_back(f[b]);
        for(auto& nb: nbr) { std::sort(nb.begin(), nb.end()); nb.erase(std::unique(nb.begin(), nb.end()), nb.end()); }

        // Explicit combinatorial Laplacian L, then K += lambda * L^T L on
        // the free block (fixed rows/columns vanish since z_fixed = 0).
        std::vector<std::vector<double>> L(n, std::vector<double>(n, 0.0));
        for(int i=0;i<n;++i)
        {
            L[i][i] = 1.0;
            for(int j: nbr[i]) L[i][j] = -1.0/(double)nbr[i].size();
        }
        std::vector<std::vector<double>> K(nf, std::vector<double>(nf, 0.0));
        for(int a=0;a<nf;++a)
        {
            K[a][a] += fx.mu;
            for(int b=0;b<nf;++b)
            {
                double s = 0.0;
                for(int k=0;k<n;++k) s += L[k][free_v[a]]*L[k][free_v[b]];
                K[a][b] += fx.lambda*s;
            }
        }

        active_out = 0;
        std::array<std::vector<double>,3> rhs;
        for(auto& r: rhs) r.assign(nf, 0.0);
        static const std::array<double,3> pat[3] = {
            {{2.0/3.0,1.0/6.0,1.0/6.0}}, {{1.0/6.0,2.0/3.0,1.0/6.0}},
            {{1.0/6.0,1.0/6.0,2.0/3.0}}};
        for(auto f: fx.faces)
        {
            const double area = 0.5*length(cross(x[f[1]]-x[f[0]], x[f[2]]-x[f[0]]));
            for(int p=0;p<3;++p)
            {
                Vec3d q(0.0);
                for(int a=0;a<3;++a) q += pat[p][a]*x[f[a]];
                BruteHit hit = brute_closest(fx.target_tris, q);
                if(!(hit.sq_dist < fx.d*fx.d)) continue;
                ++active_out;
                const double w = area/3.0;
                Vec3d q0(0.0);
                for(int a=0;a<3;++a) q0 += pat[p][a]*x[f[a]]; // x == X0 on first iteration
                for(int a=0;a<3;++a)
                {
                    const int ca = free_map[f[a]];
                    if(ca < 0) continue;
                    for(int b=0;b<3;++b)
                    {
                        const int cb = free_map[f[b]];
                        if(cb >= 0) K[ca][cb] += w*pat[p][a]*pat[p][b];
                    }
                    for(int k=0;k<3;++k)
                        rhs[k][ca] += w*pat[p][a]*(hit.point[k]-q0[k]);
                }
            }
        }

        std::vector<Vec3d> proposal = x;
        for(int i=0;i<n;++i) if(is_fixed[i]) proposal[i] = x[i];
        for(int k=0;k<3;++k)
        {
            std::vector<double> z = dense_solve(K, rhs[k]);
            for(int c=0;c<nf;++c) proposal[free_v[c]][k] = x[free_v[c]][k] + z[c];
        }
        return proposal;
    }

    int system_dense_proposal()
    {
        FitFixture fx;
        fx.build(4, 0.12);
        SurfaceFitOptions opt;
        opt.lambda = fx.lambda; opt.mu = fx.mu; opt.d = fx.d;
        for(int idx: fx.fixed) opt.fixed_vertices.push_back(VertexID(idx));
        SurfaceFitSession session(fx.source, fx.target, opt);

        const std::vector<Vec3d> x0 = snapshot_positions(fx.source);
        std::size_t ref_active = 0;
        const std::vector<Vec3d> proposal = reference_frozen_solve(fx, x0, ref_active);

        SurfaceFitReport rep = session.iterate(200);
        TASK_CHECK(rep.status == SurfaceFitStatus::progress);
        TASK_CHECK(rep.alpha == 1.0);
        TASK_CHECK(rep.active_samples == ref_active);
        TASK_CHECK(rep.active_samples == 3*fx.faces.size());
        TASK_CHECK(rep.sample_count == 3*fx.faces.size());

        const std::vector<Vec3d> after = snapshot_positions(fx.source);
        for(int idx: fx.fixed)
            TASK_CHECK(vec_close(after[idx], x0[idx], 0.0)); // exact hard constraint
        for(std::size_t i=0;i<after.size();++i)
            TASK_CHECK(vec_close(after[i], proposal[i], 1e-8));
        TASK_CHECK(rep.residual <= 1e-8);
        return 0;
    }

    int system_energy_and_alpha()
    {
        FitFixture fx;
        fx.build(4, 0.1);
        SurfaceFitOptions opt;
        opt.lambda = fx.lambda; opt.mu = fx.mu; opt.d = fx.d;
        for(int idx: fx.fixed) opt.fixed_vertices.push_back(VertexID(idx));
        SurfaceFitSession session(fx.source, fx.target, opt);

        std::vector<Vec3d> x_before = snapshot_positions(fx.source);
        SurfaceFitReport rep = session.iterate(200);
        TASK_CHECK(rep.status == SurfaceFitStatus::progress);
        std::vector<Vec3d> x_after = snapshot_positions(fx.source);

        // Independently recompute the full truncated energy (data term via
        // exhaustive projection, regularizer straight from the definition).
        const int n = (int)x_before.size();
        std::vector<std::vector<int>> nbr(n);
        for(auto f: fx.faces)
            for(int a=0;a<3;++a) for(int b=0;b<3;++b)
                if(a != b) nbr[f[a]].push_back(f[b]);
        for(auto& nb: nbr) { std::sort(nb.begin(), nb.end()); nb.erase(std::unique(nb.begin(), nb.end()), nb.end()); }
        // Sample weights are fixed at the initial area and never recomputed.
        std::vector<double> area0;
        for(auto f: fx.faces)
            area0.push_back(0.5*length(cross(x_before[f[1]]-x_before[f[0]],
                                             x_before[f[2]]-x_before[f[0]])));
        auto full_energy = [&](const std::vector<Vec3d>& x) {
            double e = 0.0;
            static const std::array<double,3> pat[3] = {
                {{2.0/3.0,1.0/6.0,1.0/6.0}}, {{1.0/6.0,2.0/3.0,1.0/6.0}},
                {{1.0/6.0,1.0/6.0,2.0/3.0}}};
            for(std::size_t fi=0;fi<fx.faces.size();++fi)
            {
                const auto& f = fx.faces[fi];
                for(int p=0;p<3;++p)
                {
                    Vec3d q(0.0);
                    for(int a=0;a<3;++a) q += pat[p][a]*x[f[a]];
                    e += area0[fi]/3.0*std::min(brute_closest(fx.target_tris, q).sq_dist, fx.d*fx.d);
                }
            }
            for(int i=0;i<n;++i)
            {
                e += fx.mu*sqr_length(x[i]-x_before[i]);
                Vec3d mean(0.0);
                for(int j: nbr[i]) mean += x[j]-x_before[j];
                mean /= (double)nbr[i].size();
                e += fx.lambda*sqr_length((x[i]-x_before[i])-mean);
            }
            return e;
        };
        TASK_CHECK(close(rep.energy_before, full_energy(x_before), 1e-10));
        TASK_CHECK(close(rep.energy_after, full_energy(x_after), 1e-10));
        TASK_CHECK(rep.energy_after < rep.energy_before);

        // The accepted positions must equal X_before + alpha*(proposal-X_before).
        std::size_t active = 0;
        const std::vector<Vec3d> proposal = reference_frozen_solve(fx, x_before, active);
        TASK_CHECK(active == rep.active_samples);
        bool power_of_half = false;
        for(int k=0;k<=24;++k) if(close(rep.alpha, std::ldexp(1.0,-k), 1e-12)) power_of_half = true;
        TASK_CHECK(power_of_half);
        for(std::size_t i=0;i<x_after.size();++i)
        {
            const Vec3d expected = x_before[i] + rep.alpha*(proposal[i]-x_before[i]);
            TASK_CHECK(vec_close(x_after[i], expected, 1e-8));
        }
        return 0;
    }

    int system_invalid_inputs()
    {
        Manifold src, tgt;
        build_grid(src, 3, [](int i,int j){ return Vec3d(i,j,0); });
        build_grid(tgt, 3, [](int i,int j){ return Vec3d(i,j,1); });

        auto throws_invalid = [&](SurfaceFitOptions o) {
            bool threw = false;
            try { SurfaceFitSession s(src, tgt, o); }
            catch(const std::invalid_argument&) { threw = true; }
            TASK_CHECK(threw);
        };
        SurfaceFitOptions o;
        o.lambda = 0.0; throws_invalid(o);
        o.lambda = 1.0; o.mu = -1.0; throws_invalid(o);
        o.mu = 1.0; o.d = std::numeric_limits<double>::infinity(); throws_invalid(o);
        o.d = 1.0; o.fixed_vertices = {VertexID(9999)}; throws_invalid(o);
        o.fixed_vertices = {VertexID(0), VertexID(0)}; throws_invalid(o);

        Manifold empty;
        bool threw = false;
        try { SurfaceFitSession s(empty, tgt, SurfaceFitOptions{}); }
        catch(const std::invalid_argument&) { threw = true; }
        TASK_CHECK(threw);
        threw = false;
        try { SurfaceFitSession s(src, empty, SurfaceFitOptions{}); }
        catch(const std::invalid_argument&) { threw = true; }
        TASK_CHECK(threw);

        // A rejected constructor must not have moved the source positions.
        const std::vector<Vec3d> before = snapshot_positions(src);
        SurfaceFitOptions bad;
        bad.fixed_vertices = {VertexID(123456)};
        threw = false;
        try { SurfaceFitSession s(src, tgt, bad); }
        catch(const std::invalid_argument&) { threw = true; }
        TASK_CHECK(threw);
        TASK_CHECK(snapshot_positions(src) == before);

        SurfaceFitSession good(src, tgt, SurfaceFitOptions{});
        threw = false;
        try { good.iterate(0); }
        catch(const std::invalid_argument&) { threw = true; }
        TASK_CHECK(threw);
        return 0;
    }

    // ---------------------------------------------------------------
    // Iteration group.
    // ---------------------------------------------------------------
    int iteration_truncated_outliers()
    {
        // Target only covers the x >= 0 half of the source footprint.
        Manifold src;
        build_grid(src, 5, [](int i,int j) {
            return Vec3d(-1.0+2.0*i/4, -1.0+2.0*j/4, 0.5);
        });
        Manifold tgt;
        build_grid(tgt, 5, [](int i,int j) {
            return Vec3d(0.0+1.0*i/4, -1.0+2.0*j/4, 0.0);
        });
        SurfaceFitOptions opt;
        opt.d = 0.6; // stand-off is 0.5: near half matches, far half truncates
        opt.lambda = 0.01;
        opt.mu = 1e-4;
        SurfaceFitSession session(src, tgt, opt);
        const std::size_t total = session.sample_count();
        SurfaceFitReport rep = session.iterate(100);
        TASK_CHECK(rep.active_samples > 0);
        TASK_CHECK(rep.active_samples < total);
        TASK_CHECK(rep.status == SurfaceFitStatus::progress);

        // The truncated samples contribute the constant w*d^2: recompute
        // the data objective with the exhaustive kernel, using the fixed
        // initial per-face weights.
        std::vector<TriD> tris = snapshot_triangles(tgt);
        std::vector<double> w0;
        for(auto f: src.faces()) w0.push_back(src.area(f)); // before first iterate, area == A_f
        double ref_data = 0.0;
        int fi = 0;
        for(auto f: src.faces())
        {
            std::array<VertexID,3> fv{};
            int k=0;
            for(auto vid: src.incident_vertices(f)) fv[k++] = vid;
            const double w = w0[fi++]/3.0;
            static const std::array<double,3> pat[3] = {
                {{2.0/3.0,1.0/6.0,1.0/6.0}}, {{1.0/6.0,2.0/3.0,1.0/6.0}},
                {{1.0/6.0,1.0/6.0,2.0/3.0}}};
            for(int p=0;p<3;++p)
            {
                Vec3d q(0.0);
                for(int a=0;a<3;++a) q += pat[p][a]*src.pos(fv[a]);
                ref_data += w*std::min(brute_closest(tris, q).sq_dist, opt.d*opt.d);
            }
        }
        TASK_CHECK(rep.energy_after >= ref_data - 1e-12);
        TASK_CHECK(rep.energy_after < rep.energy_before);
        return 0;
    }

    int iteration_repair_across_faces()
    {
        // Curved bowl target tiled into many faces. The flat source starts
        // offset above the bowl; as it slides inward, sample matches must
        // jump from one target face to neighbouring faces (re-pairing).
        const int M = 10;
        const double R = 8.0;
        auto bowl = [&](double x, double y) {
            return Vec3d(x, y, -0.15*(x*x+y*y));
        };
        Manifold tgt;
        build_grid(tgt, M+1, [&](int i, int j) {
            const double x = -R + 2.0*R*i/M;
            const double y = -R + 2.0*R*j/M;
            return bowl(x, y);
        });
        const std::vector<TriD> tris = snapshot_triangles(tgt);

        Manifold src;
        const int n = 5;
        build_grid(src, n, [](int i, int j) {
            return Vec3d(2.5+i/4.0, -0.5+j/4.0, 0.5);
        });
        SurfaceFitOptions opt;
        opt.d = 20.0;
        opt.lambda = 1e-4;
        opt.mu = 1e-9;
        SurfaceFitSession session(src, tgt, opt);

        // Exhaustively assign every actual surface sample (the 2/3-pattern
        // points) to a target face, both before and after iterations.
        static const std::array<double,3> pat[3] = {
            {{2.0/3.0,1.0/6.0,1.0/6.0}}, {{1.0/6.0,2.0/3.0,1.0/6.0}},
            {{1.0/6.0,1.0/6.0,2.0/3.0}}};
        auto sample_ids = [&]() {
            std::vector<uint64_t> ids;
            for(auto f: src.faces())
            {
                std::array<VertexID,3> fv{};
                int k=0;
                for(auto vid: src.incident_vertices(f)) fv[k++] = vid;
                for(int p=0;p<3;++p)
                {
                    Vec3d q(0.0);
                    for(int a=0;a<3;++a) q += pat[p][a]*src.pos(fv[a]);
                    ids.push_back(brute_closest(tris, q).id);
                }
            }
            return ids;
        };
        const std::vector<uint64_t> ids_before = sample_ids();
        TASK_CHECK(ids_before.size() == session.sample_count());

        bool repaired = false;
        for(int it=0; it<10; ++it)
        {
            SurfaceFitReport rep = session.iterate(300);
            TASK_CHECK(std::isfinite(rep.energy_after));
            const std::vector<uint64_t> ids_now = sample_ids();
            std::size_t changed = 0;
            for(std::size_t k=0;k<ids_now.size();++k)
                if(ids_now[k] != ids_before[k]) ++changed;
            if(changed >= 3) { repaired = true; break; }
            if(rep.status != SurfaceFitStatus::progress) break;
        }
        TASK_CHECK(repaired);
        TASK_CHECK(valid(src));
        return 0;
    }

    int iteration_backtracking()
    {
        // Boundary ring pinned, target shifted laterally and lowered: the
        // full Gauss-Newton step overshoots the interior and flips faces,
        // so the line search must backtrack to a shorter half-power step.
        const int n = 5;
        Manifold src;
        build_grid(src, n, [](int i, int j) {
            return Vec3d(-1.0+2.0*i/(n-1), -1.0+2.0*j/(n-1), 0.0);
        });
        Manifold tgt;
        build_grid(tgt, n, [](int i, int j) {
            return Vec3d(-1.0+2.0*i/(n-1)+1.2, -1.0+2.0*j/(n-1), -2.0);
        });

        // Independent record of the initial area vectors.
        std::vector<Vec3d> area0;
        for(auto f: src.faces())
        {
            std::array<Vec3d,3> pp{};
            int k=0;
            for(auto vid: src.incident_vertices(f)) pp[k++] = src.pos(vid);
            area0.push_back(cross(pp[1]-pp[0], pp[2]-pp[0]));
        }

        SurfaceFitOptions opt;
        opt.d = 20.0;
        opt.lambda = 1e-8;
        opt.mu = 1e-11;
        for(auto v: src.vertices())
            if(src.boundary(v)) opt.fixed_vertices.push_back(v);
        SurfaceFitSession session(src, tgt, opt);

        SurfaceFitReport rep = session.iterate(300);
        TASK_CHECK(rep.status == SurfaceFitStatus::progress);
        TASK_CHECK(rep.alpha < 1.0);
        bool power_of_half = false;
        for(int k=1;k<=24;++k) if(close(rep.alpha, std::ldexp(1.0,-k), 1e-12)) power_of_half = true;
        TASK_CHECK(power_of_half);
        TASK_CHECK(rep.energy_after < rep.energy_before);

        // The accepted step keeps a positive dot product with every old area.
        int fi = 0;
        for(auto f: src.faces())
        {
            std::array<Vec3d,3> pp{};
            int k=0;
            for(auto vid: src.incident_vertices(f)) pp[k++] = src.pos(vid);
            const Vec3d new_area = cross(pp[1]-pp[0], pp[2]-pp[0]);
            TASK_CHECK(dot(new_area, area0[fi++]) > 0.0);
            TASK_CHECK(sqr_length(new_area) > 0.0);
        }
        TASK_CHECK(valid(src));
        return 0;
    }

    int iteration_all_fixed()
    {
        Manifold src;
        build_grid(src, 4, [](int i,int j){ return Vec3d(i,j,0); });
        Manifold tgt;
        build_grid(tgt, 4, [](int i,int j){ return Vec3d(i,j,2); });
        SurfaceFitOptions opt;
        opt.d = 5.0;
        for(auto v: src.vertices()) opt.fixed_vertices.push_back(v);
        const std::vector<Vec3d> before = snapshot_positions(src);
        SurfaceFitSession session(src, tgt, opt);
        SurfaceFitReport rep = session.iterate(50);
        TASK_CHECK(rep.status == SurfaceFitStatus::stationary);
        TASK_CHECK(rep.alpha == 0.0);
        TASK_CHECK(rep.energy_after == rep.energy_before);
        TASK_CHECK(snapshot_positions(src) == before);
        TASK_CHECK(rep.active_samples == rep.sample_count);
        return 0;
    }

    int iteration_cg_fail_and_recover()
    {
        Manifold src;
        build_grid(src, 8, [](int i,int j){ return Vec3d(i,j,0); });
        Manifold tgt;
        build_grid(tgt, 8, [](int i,int j){ return Vec3d(i,j,0.4); });
        SurfaceFitOptions opt;
        opt.d = 10.0;
        for(auto v: src.vertices()) if(src.boundary(v)) opt.fixed_vertices.push_back(v);
        SurfaceFitSession session(src, tgt, opt);
        const std::vector<Vec3d> before = snapshot_positions(src);

        bool threw = false;
        try { session.iterate(1); }
        catch(const std::runtime_error&) { threw = true; }
        TASK_CHECK(threw);
        // Failed solve leaves the source untouched.
        TASK_CHECK(snapshot_positions(src) == before);

        SurfaceFitReport rep = session.iterate(500);
        TASK_CHECK(rep.status == SurfaceFitStatus::progress);
        TASK_CHECK(rep.residual <= 1e-8);
        TASK_CHECK(rep.energy_after < rep.energy_before);
        return 0;
    }

    int iteration_continuous()
    {
        Manifold src;
        build_grid(src, 6, [](int i,int j) {
            const double x=-1.0+2.0*i/5, y=-1.0+2.0*j/5;
            return Vec3d(x, y, -0.4);
        });
        Manifold tgt;
        build_grid(tgt, 6, [](int i,int j) {
            const double x=-1.0+2.0*i/5, y=-1.0+2.0*j/5;
            return Vec3d(x, y, 0.35*std::sin(2.0*x)*std::cos(1.5*y));
        });
        SurfaceFitOptions opt;
        opt.d = 5.0;
        for(auto v: src.vertices()) if(src.boundary(v)) opt.fixed_vertices.push_back(v);
        SurfaceFitSession session(src, tgt, opt);

        double previous = std::numeric_limits<double>::infinity();
        int progress_rounds = 0;
        for(int it=0;it<8;++it)
        {
            SurfaceFitReport rep = session.iterate(150);
            TASK_CHECK(std::isfinite(rep.energy_before));
            TASK_CHECK(std::isfinite(rep.energy_after));
            TASK_CHECK(rep.energy_after <= rep.energy_before);
            TASK_CHECK(rep.energy_after <= previous*(1.0+1e-15));
            previous = rep.energy_after;
            if(rep.status == SurfaceFitStatus::progress) ++progress_rounds;
            for(auto v: src.vertices())
                for(int k=0;k<3;++k) TASK_CHECK(std::isfinite(src.pos(v)[k]));
        }
        TASK_CHECK(progress_rounds >= 2);
        TASK_CHECK(valid(src));
        return 0;
    }

    int iteration_target_released()
    {
        Manifold src;
        build_grid(src, 4, [](int i,int j){ return Vec3d(i,j,0.6); });
        std::vector<TriD> kept;
        {
            Manifold* tgt = new Manifold();
            build_grid(*tgt, 4, [](int i,int j){ return Vec3d(i,j,0.0); });
            kept = snapshot_triangles(*tgt);
            SurfaceFitOptions opt;
            opt.d = 5.0;
            SurfaceFitSession session(src, *tgt, opt);
            delete tgt; // target gone before any iteration

            SurfaceFitReport rep = session.iterate(120);
            TASK_CHECK(rep.status == SurfaceFitStatus::progress ||
                       rep.status == SurfaceFitStatus::stationary);
            double ref_energy = 0.0;
            for(auto f: src.faces())
            {
                const double area = src.area(f);
                static const std::array<double,3> pat[3] = {
                    {{2.0/3.0,1.0/6.0,1.0/6.0}}, {{1.0/6.0,2.0/3.0,1.0/6.0}},
                    {{1.0/6.0,1.0/6.0,2.0/3.0}}};
                std::array<VertexID,3> fv{};
                int k=0;
                for(auto vid: src.incident_vertices(f)) fv[k++] = vid;
                for(int p=0;p<3;++p)
                {
                    Vec3d q(0.0);
                    for(int a=0;a<3;++a) q += pat[p][a]*src.pos(fv[a]);
                    ref_energy += area/3.0*std::min(brute_closest(kept, q).sq_dist, opt.d*opt.d);
                }
            }
            TASK_CHECK(rep.status == SurfaceFitStatus::stationary ||
                       rep.energy_after >= ref_energy - 1e-10);
        }
        return 0;
    }
}

#define TASK_CHECK_THROW_BEGIN_LOCAL_GUARD
#undef TASK_CHECK_THROW_BEGIN_LOCAL_GUARD

extern const struct task_case task_cases[];
const struct task_case task_cases[] = {
    {"witness", "cp_face", witness_face},
    {"witness", "cp_edge", witness_edge},
    {"witness", "cp_vertex", witness_vertex},
    {"witness", "cp_tie", witness_tie},
    {"witness", "cp_sparse_slots", witness_sparse_slots},
    {"witness", "cp_double_bias", witness_double_bias},
    {"witness", "cp_empty_witness", witness_empty_preserves_witness},
    {"witness", "cp_invalid_args", witness_invalid_arguments},
    {"witness", "cp_failed_build_keeps_old", witness_failed_build_keeps_old_tree},
    {"witness", "tree_pruning", witness_pruning},
    {"system", "dense_proposal", system_dense_proposal},
    {"system", "energy_and_alpha", system_energy_and_alpha},
    {"system", "invalid_inputs", system_invalid_inputs},
    {"iteration", "truncated_outliers", iteration_truncated_outliers},
    {"iteration", "repair_across_faces", iteration_repair_across_faces},
    {"iteration", "backtracking", iteration_backtracking},
    {"iteration", "all_fixed", iteration_all_fixed},
    {"iteration", "cg_fail_recover", iteration_cg_fail_and_recover},
    {"iteration", "continuous", iteration_continuous},
    {"iteration", "target_released", iteration_target_released},
};
const size_t task_case_count = sizeof(task_cases)/sizeof(task_cases[0]);
