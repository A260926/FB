/* ----------------------------------------------------------------------- *
 * This file is part of GEL, http://www.imm.dtu.dk/GEL
 * Copyright (C) the authors and DTU Informatics
 * For license and list of authors, see ../../doc/intro.pdf
 * ----------------------------------------------------------------------- */

/**
 * @file SurfaceFit.cpp
 * @brief Sampling-density independent surface fitting session.
 */

#include <GEL/HMesh/SurfaceFit.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <unordered_set>

#include <GEL/Geometry/AABox.h>
#include <GEL/Geometry/BoundingTree.h>

using namespace CGLA;

namespace HMesh
{
    namespace
    {
        constexpr double kCoordBound = 100.0;
        constexpr double kStationaryInf = 1e-10;
        constexpr double kCgTolerance = 1e-8;
        constexpr int kMaxLineSearchSteps = 24;

        struct Sample
        {
            int face = -1;
            std::array<int, 3> vert{};
            std::array<double, 3> bary{};
            double weight = 0.0;
        };

        struct Query
        {
            double sq_dist = 0.0;
            Vec3d point;
        };

        void require_finite_mesh(const Manifold& m, const char* tag)
        {
            if(m.no_vertices() == 0 || m.no_faces() == 0)
                throw std::invalid_argument(std::string("SurfaceFit: empty ") + tag + " mesh");
            if(!valid(m))
                throw std::invalid_argument(std::string("SurfaceFit: invalid manifold in ") + tag);
            for(auto v: m.vertices())
            {
                const Vec3d p = m.pos(v);
                for(int k=0;k<3;++k)
                {
                    if(!std::isfinite(p[k]))
                        throw std::invalid_argument("SurfaceFit: non-finite coordinate");
                    if(std::fabs(p[k]) > kCoordBound)
                        throw std::invalid_argument("SurfaceFit: coordinate exceeds bound of 100");
                }
            }
            // Every face must be a non-degenerate triangle and every vertex
            // must belong to a face (no isolated points).
            std::vector<char> touched(m.allocated_vertices(), 0);
            for(auto f: m.faces())
            {
                if(m.no_edges(f) != 3)
                    throw std::invalid_argument("SurfaceFit: target/source faces must be triangles");
                Vec3d v[3];
                int k = 0;
                for(auto vid: m.incident_vertices(f))
                {
                    v[k] = m.pos(vid);
                    if(vid.get_index() >= touched.size())
                        touched.resize(vid.get_index()+1, 0);
                    touched[vid.get_index()] = 1;
                    ++k;
                }
                if(sqr_length(cross(v[1]-v[0], v[2]-v[0])) <= 0.0)
                    throw std::invalid_argument("SurfaceFit: degenerate triangle");
            }
            for(auto vid: m.vertices())
                if(!touched[vid.get_index()])
                    throw std::invalid_argument("SurfaceFit: isolated vertex");
        }
    }

    struct SurfaceFitSession::Impl
    {
        Manifold& source;
        int nverts = 0;
        std::vector<VertexID> verts;
        std::vector<std::array<int, 3>> faces;
        std::vector<Vec3d> x0;
        std::vector<double> face_area0;
        std::vector<Sample> samples;
        std::vector<std::vector<int>> neighbors;
        std::vector<char> fixed;
        std::vector<int> free_map;
        std::vector<int> free_verts;
        double lambda = 1.0, mu = 1.0, d = 1.0;

        Geometry::BoundingTree<Geometry::AABox> tree;
        std::size_t total_node_visits = 0;
        std::size_t total_triangle_tests = 0;

        Impl(Manifold& src): source(src) {}

        Vec3d sample_point(const Sample& s, const std::vector<Vec3d>& x) const
        {
            return s.bary[0]*x[s.vert[0]]
                 + s.bary[1]*x[s.vert[1]]
                 + s.bary[2]*x[s.vert[2]];
        }

        // Query the frozen target hierarchy for every sample.
        std::vector<Query> query_all(const std::vector<Vec3d>& x)
        {
            std::vector<Query> queries(samples.size());
            for(std::size_t s=0;s<samples.size();++s)
            {
                Geometry::ClosestPointResult hit;
                if(!tree.closest_point(sample_point(samples[s], x), hit))
                    throw std::runtime_error("SurfaceFit: empty target hierarchy");
                queries[s].sq_dist = hit.sq_dist;
                queries[s].point = hit.point;
                total_node_visits += hit.node_visits;
                total_triangle_tests += hit.triangle_tests;
            }
            return queries;
        }

        double regularizer_energy(const std::vector<Vec3d>& x) const
        {
            double e = 0.0;
            for(int i=0;i<nverts;++i)
            {
                const Vec3d z = x[i]-x0[i];
                e += mu * sqr_length(z);
                Vec3d mean(0.0);
                for(int j: neighbors[i]) mean += x[j]-x0[j];
                if(!neighbors[i].empty()) mean /= static_cast<double>(neighbors[i].size());
                const Vec3d lz = z - mean;
                e += lambda * sqr_length(lz);
            }
            return e;
        }

        double data_energy(const std::vector<Query>& queries) const
        {
            const double d2 = d*d;
            double e = 0.0;
            for(std::size_t s=0;s<samples.size();++s)
                e += samples[s].weight * std::min(queries[s].sq_dist, d2);
            return e;
        }

        // Matrix-free product with the frozen normal-equation matrix K on
        // the free unknowns (one coordinate). Fixed unknowns are eliminated
        // and contribute only through the right hand side.
        void matvec(const std::vector<double>& u, std::vector<double>& out,
                    const std::vector<int>& comp_of,
                    const std::vector<Sample>& active) const
        {
            const int nfree = static_cast<int>(free_verts.size());
            std::fill(out.begin(), out.end(), 0.0);
            std::vector<double> z(nverts, 0.0);
            for(int c=0;c<nfree;++c) z[free_verts[c]] = u[c];

            // Frozen data term.
            for(std::size_t si=0;si<active.size();++si)
            {
                const Sample& s = active[si];
                double r = 0.0;
                for(int a=0;a<3;++a)
                {
                    const int c = comp_of[s.vert[a]];
                    if(c >= 0) r += s.bary[a]*u[c];
                }
                for(int a=0;a<3;++a)
                {
                    const int c = comp_of[s.vert[a]];
                    if(c >= 0) out[c] += s.weight*s.bary[a]*r;
                }
            }

            // Laplacian term lambda * L^T L, with fixed vertices at z = 0.
            std::vector<double> lz(nverts, 0.0);
            for(int i=0;i<nverts;++i)
            {
                double mean = 0.0;
                for(int j: neighbors[i]) mean += z[j];
                if(!neighbors[i].empty()) mean /= static_cast<double>(neighbors[i].size());
                lz[i] = z[i]-mean;
            }
            for(int i=0;i<nverts;++i)
            {
                double v = lz[i];
                for(int j: neighbors[i])
                    v -= lz[j]/static_cast<double>(neighbors[j].size());
                const int c = comp_of[i];
                if(c >= 0) out[c] += lambda*v;
            }

            // Pull-to-X0 term.
            for(int c=0;c<nfree;++c) out[c] += mu*u[c];
        }
    };

    SurfaceFitSession::SurfaceFitSession(Manifold& source, const Manifold& target,
                                         const SurfaceFitOptions& options)
    {
        if(!(std::isfinite(options.lambda) && options.lambda > 0.0) ||
           !(std::isfinite(options.mu) && options.mu > 0.0) ||
           !(std::isfinite(options.d) && options.d > 0.0))
            throw std::invalid_argument("SurfaceFit: lambda, mu and d must be positive finite numbers");

        require_finite_mesh(source, "source");
        require_finite_mesh(target, "target");

        impl = new Impl(source);
        try
        {
        impl->lambda = options.lambda;
        impl->mu = options.mu;
        impl->d = options.d;

        // Fixed handles: valid, in use and unique.
        std::unordered_set<VertexID> seen_fixed;
        for(VertexID v: options.fixed_vertices)
        {
            if(!source.in_use(v))
                throw std::invalid_argument("SurfaceFit: fixed vertex handle is not in use");
            if(!seen_fixed.insert(v).second)
                throw std::invalid_argument("SurfaceFit: duplicate fixed vertex handle");
        }

        impl->nverts = static_cast<int>(source.no_vertices());
        impl->verts.assign(source.vertices_begin(), source.vertices_end());
        VertexAttributeVector<int> local_index(-1);
        for(int i=0;i<impl->nverts;++i)
            local_index[impl->verts[i]] = i;

        impl->x0.resize(impl->nverts);
        for(int i=0;i<impl->nverts;++i)
            impl->x0[i] = source.pos(impl->verts[i]);

        impl->fixed.assign(impl->nverts, 0);
        for(VertexID v: options.fixed_vertices)
            impl->fixed[local_index[v]] = 1;

        // Connectivity and per-face initial geometry.
        impl->neighbors.resize(impl->nverts);
        for(int i=0;i<impl->nverts;++i)
            for(auto nv: source.incident_vertices(impl->verts[i]))
                impl->neighbors[i].push_back(local_index[nv]);

        static const std::array<double,3> kPattern[3] = {
            {{2.0/3.0, 1.0/6.0, 1.0/6.0}},
            {{1.0/6.0, 2.0/3.0, 1.0/6.0}},
            {{1.0/6.0, 1.0/6.0, 2.0/3.0}}
        };

        for(auto f: source.faces())
        {
            std::array<int,3> fv{};
            int k = 0;
            for(auto vid: source.incident_vertices(f))
                fv[k++] = local_index[vid];
            const double area = source.area(f);
            if(!(area > 0.0))
                throw std::invalid_argument("SurfaceFit: degenerate source face");
            impl->faces.push_back(fv);
            impl->face_area0.push_back(area);
            const int face_idx = static_cast<int>(impl->faces.size())-1;
            for(int p=0;p<3;++p)
            {
                Sample s;
                s.face = face_idx;
                s.vert = fv;
                s.bary = kPattern[p];
                s.weight = area/3.0;
                impl->samples.push_back(s);
            }
        }

        impl->free_map.assign(impl->nverts, -1);
        for(int i=0;i<impl->nverts;++i)
            if(!impl->fixed[i])
            {
                impl->free_map[i] = static_cast<int>(impl->free_verts.size());
                impl->free_verts.push_back(i);
            }

        // Independent double precision snapshot of the target surface.
        std::vector<Geometry::SurfaceTriangle> target_tris;
        target_tris.reserve(target.no_faces());
        std::unordered_set<uint64_t> used_ids;
        for(auto f: target.faces())
        {
            Vec3d v[3];
            int k = 0;
            for(auto vid: target.incident_vertices(f))
                v[k++] = target.pos(vid);
            const uint64_t id = f.get_index()+1;
            if(id == 0 || !used_ids.insert(id).second)
                throw std::invalid_argument("SurfaceFit: invalid target face id");
            target_tris.emplace_back(id, v[0], v[1], v[2]);
        }
        // The hierarchy is built exactly once; a failed build leaves no
        // partial tree behind (the member was empty until now).
        impl->tree.build_surface(std::move(target_tris));
        }
        catch(...)
        {
            // No session object is constructed on a throw, so the manually
            // owned implementation must be cleaned up here.
            delete impl;
            throw;
        }
    }

    SurfaceFitSession::~SurfaceFitSession()
    {
        delete impl;
    }

    std::size_t SurfaceFitSession::vertex_count() const { return impl->nverts; }
    std::size_t SurfaceFitSession::sample_count() const { return impl->samples.size(); }

    SurfaceFitReport SurfaceFitSession::iterate(int max_cg)
    {
        if(max_cg <= 0)
            throw std::invalid_argument("SurfaceFit: max_cg must be positive");

        Impl& im = *impl;
        const int n = im.nverts;
        const int nfree = static_cast<int>(im.free_verts.size());
        const double d2 = im.d*im.d;

        // Current positions come straight from the source mesh.
        std::vector<Vec3d> x(n);
        for(int i=0;i<n;++i)
        {
            x[i] = im.source.pos(im.verts[i]);
            for(int k=0;k<3;++k)
                if(!std::isfinite(x[i][k]))
                    throw std::runtime_error("SurfaceFit: non-finite source position");
        }

        im.total_node_visits = 0;
        im.total_triangle_tests = 0;

        // Matching pass from the current positions.
        std::vector<Query> queries = im.query_all(x);
        const double energy_before = im.data_energy(queries) + im.regularizer_energy(x);

        // Frozen quadratic data term: only strict sub-threshold matches enter.
        std::vector<Sample> active;
        std::vector<Vec3d> active_target;
        for(std::size_t s=0;s<im.samples.size();++s)
        {
            if(queries[s].sq_dist < d2)
            {
                active.push_back(im.samples[s]);
                active_target.push_back(queries[s].point);
            }
        }

        SurfaceFitReport report;
        report.energy_before = energy_before;
        report.active_samples = active.size();
        report.sample_count = im.samples.size();

        // Diagonal of K for the Jacobi preconditioner (mu > 0 keeps it
        // strictly positive even with no active samples).
        std::vector<double> diag(nfree, im.mu);
        for(int i=0;i<n;++i)
        {
            if(im.free_map[i] < 0) continue;
            double lap_diag = 1.0;
            for(int j: im.neighbors[i])
                lap_diag += 1.0/std::pow(static_cast<double>(im.neighbors[j].size()), 2.0);
            diag[im.free_map[i]] += im.lambda*lap_diag;
        }
        for(std::size_t si=0;si<active.size();++si)
        {
            const Sample& s = active[si];
            for(int a=0;a<3;++a)
            {
                const int c = im.free_map[s.vert[a]];
                if(c >= 0) diag[c] += s.weight*s.bary[a]*s.bary[a];
            }
        }

        // Right hand side per coordinate, with fixed corner positions at X0
        // moved to the right hand side.
        std::array<std::vector<double>,3> rhs;
        for(int k=0;k<3;++k) rhs[k].assign(nfree, 0.0);
        for(std::size_t si=0;si<active.size();++si)
        {
            const Sample& s = active[si];
            // Sample position when every free displacement z is zero:
            // all corners at X0.
            Vec3d q0(0.0);
            for(int a=0;a<3;++a) q0 += s.bary[a]*im.x0[s.vert[a]];
            for(int a=0;a<3;++a)
            {
                const int ca = im.free_map[s.vert[a]];
                if(ca < 0) continue;
                for(int k=0;k<3;++k)
                    rhs[k][ca] += s.weight*s.bary[a]*(active_target[si][k]-q0[k]);
            }
        }

        // Matrix-free CG, one coordinate at a time, sharing the same K.
        std::array<std::vector<double>,3> zfree;
        double worst_residual = 0.0;
        for(int coord=0;coord<3;++coord)
        {
            std::vector<double> u(nfree, 0.0), r = rhs[coord], p(nfree), Kp(nfree), out(nfree);
            double b_inf = 0.0;
            for(double v: r) b_inf = std::max(b_inf, std::fabs(v));
            const double tol = kCgTolerance*std::max(b_inf, 1.0);

            auto residual_inf = [&]() {
                double m = 0.0;
                for(double v: r) m = std::max(m, std::fabs(v));
                return m;
            };

            for(int c=0;c<nfree;++c) p[c] = r[c]/diag[c];
            double rz = 0.0;
            for(int c=0;c<nfree;++c) rz += r[c]*p[c];

            int iter = 0;
            while(residual_inf() > tol)
            {
                if(iter >= max_cg)
                    throw std::runtime_error("SurfaceFit: CG did not converge within max_cg iterations");
                im.matvec(p, Kp, im.free_map, active);
                double pKp = 0.0;
                for(int c=0;c<nfree;++c) pKp += p[c]*Kp[c];
                if(!(pKp > 0.0) || !std::isfinite(pKp))
                    throw std::runtime_error("SurfaceFit: CG breakdown (non positive definite system)");
                const double a = rz/pKp;
                for(int c=0;c<nfree;++c)
                {
                    u[c] += a*p[c];
                    r[c] -= a*Kp[c];
                }
                ++iter;
                if(residual_inf() <= tol) break;
                double rz_new = 0.0;
                for(int c=0;c<nfree;++c)
                {
                    const double zc = r[c]/diag[c];
                    out[c] = zc;
                    rz_new += r[c]*zc;
                }
                const double beta = rz_new/rz;
                for(int c=0;c<nfree;++c) p[c] = out[c] + beta*p[c];
                rz = rz_new;
            }
            worst_residual = std::max(worst_residual, residual_inf()/std::max(b_inf, 1.0));
            for(double v: u)
                if(!std::isfinite(v))
                    throw std::runtime_error("SurfaceFit: non-finite CG solution");
            zfree[coord] = std::move(u);
        }
        report.residual = worst_residual;

        // Assemble the proposal; fixed vertices stay exactly at X0.
        std::vector<Vec3d> proposal = x;
        double move_inf = 0.0;
        for(int c=0;c<nfree;++c)
        {
            const int i = im.free_verts[c];
            proposal[i] = im.x0[i] + Vec3d(zfree[0][c], zfree[1][c], zfree[2][c]);
            for(int k=0;k<3;++k)
            {
                if(!std::isfinite(proposal[i][k]))
                    throw std::runtime_error("SurfaceFit: non-finite proposal");
                move_inf = std::max(move_inf, std::fabs(proposal[i][k]-x[i][k]));
            }
        }
        for(int i=0;i<n;++i)
            if(im.fixed[i]) proposal[i] = im.x0[i];

        if(nfree == 0 || move_inf <= kStationaryInf)
        {
            report.status = SurfaceFitStatus::stationary;
            report.alpha = 0.0;
            report.energy_after = energy_before;
            report.node_visits = im.total_node_visits;
            report.triangle_tests = im.total_triangle_tests;
            return report;
        }

        // Reference area vectors at the current (old) positions.
        std::vector<Vec3d> old_area(im.faces.size());
        for(std::size_t f=0;f<im.faces.size();++f)
        {
            const auto& fv = im.faces[f];
            old_area[f] = cross(x[fv[1]]-x[fv[0]], x[fv[2]]-x[fv[0]]);
        }

        // Backtracking line search. Trial positions live only in local
        // vectors; rejected steps never reach the mesh.
        double alpha = 1.0;
        for(int step=0; step<kMaxLineSearchSteps; ++step, alpha *= 0.5)
        {
            std::vector<Vec3d> xtry(n);
            for(int i=0;i<n;++i) xtry[i] = x[i] + alpha*(proposal[i]-x[i]);

            bool orientation_ok = true;
            for(std::size_t f=0;f<im.faces.size();++f)
            {
                const auto& fv = im.faces[f];
                const Vec3d new_area = cross(xtry[fv[1]]-xtry[fv[0]],
                                             xtry[fv[2]]-xtry[fv[0]]);
                if(dot(new_area, old_area[f]) <= 0.0)
                {
                    orientation_ok = false;
                    break;
                }
            }
            if(!orientation_ok) continue;

            std::vector<Query> trial_queries = im.query_all(xtry);
            const double energy_try = im.data_energy(trial_queries)
                                    + im.regularizer_energy(xtry);
            if(energy_try < energy_before)
            {
                for(int i=0;i<n;++i)
                {
                    for(int k=0;k<3;++k)
                        if(!std::isfinite(xtry[i][k]))
                            throw std::runtime_error("SurfaceFit: non-finite accepted position");
                    im.source.pos(im.verts[i]) = xtry[i];
                }
                report.status = SurfaceFitStatus::progress;
                report.alpha = alpha;
                report.energy_after = energy_try;
                report.node_visits = im.total_node_visits;
                report.triangle_tests = im.total_triangle_tests;
                return report;
            }
        }

        report.status = SurfaceFitStatus::no_progress;
        report.alpha = 0.0;
        report.energy_after = energy_before;
        report.node_visits = im.total_node_visits;
        report.triangle_tests = im.total_triangle_tests;
        return report;
    }
}
