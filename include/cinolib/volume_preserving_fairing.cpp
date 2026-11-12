/********************************************************************************
*  This file is part of CinoLib                                                 *
*  Copyright(C) 2016: Marco Livesu                                              *
*                                                                               *
*  The MIT License                                                              *
*                                                                               *
*  Permission is hereby granted, free of charge, to any person obtaining a      *
*  copy of this software and associated documentation files (the "Software"),   *
*  to deal in the Software without restriction, including without limitation    *
*  the rights to use, copy, modify, merge, publish, distribute, sublicense,     *
*  and/or sell copies of the Software, and to permit persons to whom the        *
*  Software is furnished to do so, subject to the following conditions:         *
*                                                                               *
*  The above copyright notice and this permission notice shall be included in   *
*  all copies or substantial portions of the Software.                          *
*                                                                               *
*  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR   *
*  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,     *
*  FITNESS FOR A PARTICULAR PURPOSE AND NON INFRINGEMENT. IN NO EVENT SHALL THE *
*  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER       *
*  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING      *
*  FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS *
*  IN THE SOFTWARE.                                                             *
*                                                                               *
*  Author(s):                                                                   *
*                                                                               *
*     Marco Livesu (marco.livesu@gmail.com)                                     *
*     http://pers.ge.imati.cnr.it/livesu/                                       *
*                                                                               *
*     Italian National Research Council (CNR)                                   *
*     Institute for Applied Mathematics and Information Technologies (IMATI)    *
*     Via de Marini, 6                                                          *
*     16149 Genoa,                                                              *
*     Italy                                                                     *
*********************************************************************************/
#include <cinolib/volume_preserving_fairing.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <queue>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace cinolib
{

namespace volume_preserving_fairing_detail
{

//::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
// This namespace is independent of the mesh attribute types (M,V,E,P): the
// public entry point is templated and must not constrain the attributes.
//::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::

CINO_INLINE
void fail(const std::string & msg)
{
    throw std::invalid_argument("volume_preserving_fairing: " + msg);
}

CINO_INLINE
bool finite_vec(const vec3d & p)
{
    return std::isfinite(p.x()) && std::isfinite(p.y()) && std::isfinite(p.z());
}

//::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::

struct Shell
{
    uint              min_vid        = 0;            // smallest vertex id
    std::vector<uint> verts;                          // incident vertices (sorted)
    std::vector<uint> faces;                          // polygons of the shell
    vec3d             ref            = vec3d(0,0,0);  // local reference point
    long double       diagonal       = 0.0L;          // bbox diagonal
    long double       initial_volume = 0.0L;          // signed volume on input
};

//::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
// All volume / energy arithmetic is performed in long double, after
// subtracting a shell-local reference point. For a *closed* surface the
// signed volume is independent of that point, so the geometry of the problem
// (and the solution it produces) is invariant under translation. Working
// after the subtraction avoids the catastrophic cancellation that would
// otherwise occur for meshes far from the origin.
//::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::

CINO_INLINE
void to_local(const vec3d & p, const vec3d & ref, long double out[3])
{
    out[0] = (long double)p.x() - (long double)ref.x();
    out[1] = (long double)p.y() - (long double)ref.y();
    out[2] = (long double)p.z() - (long double)ref.z();
}

CINO_INLINE
long double triple(const long double a[3], const long double b[3], const long double c[3])
{
    const long double ux = b[1]*c[2] - b[2]*c[1];
    const long double uy = b[2]*c[0] - b[0]*c[2];
    const long double uz = b[0]*c[1] - b[1]*c[0];
    return a[0]*ux + a[1]*uy + a[2]*uz;
}

CINO_INLINE
void cross_ld(const long double a[3], const long double b[3], long double out[3])
{
    out[0] = a[1]*b[2] - a[2]*b[1];
    out[1] = a[2]*b[0] - a[0]*b[2];
    out[2] = a[0]*b[1] - a[1]*b[0];
}

//::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::

// Signed volume of one shell, evaluated with a shell-local reference point.
CINO_INLINE
long double shell_volume(const std::vector<vec3d>                  & coords,
                         const std::vector<std::vector<uint>>       & polys,
                         const std::vector<uint>                    & faces,
                         const vec3d                                & ref)
{
    long double volume = 0.0L;
    long double a[3], b[3], c[3];
    for(uint pid : faces)
    {
        const std::vector<uint> & f = polys.at(pid);
        to_local(coords.at(f[0]), ref, a);
        to_local(coords.at(f[1]), ref, b);
        to_local(coords.at(f[2]), ref, c);
        volume += triple(a,b,c);
    }
    return volume / 6.0L;
}

//::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::

// Gradient of a shell volume w.r.t. all of its vertices, with the same
// local reference point used by shell_volume. Only the entries of the
// shell's vertices are written; every other entry is left untouched.
//
//   d/d a  det(a-r,b-r,c-r) = (b-r) x (c-r)
//   d/d b  det(a-r,b-r,c-r) = (c-r) x (a-r)
//   d/d c  det(a-r,b-r,c-r) = (a-r) x (b-r)
CINO_INLINE
void shell_volume_gradient(const std::vector<vec3d>            & coords,
                           const std::vector<std::vector<uint>> & polys,
                           const Shell                          & shell,
                           std::vector<vec3d>                   & grad)
{
    for(uint vid : shell.verts) grad.at(vid) = vec3d(0,0,0);
    long double a[3], b[3], c[3], u[3];
    for(uint pid : shell.faces)
    {
        const std::vector<uint> & f = polys.at(pid);
        to_local(coords.at(f[0]), shell.ref, a);
        to_local(coords.at(f[1]), shell.ref, b);
        to_local(coords.at(f[2]), shell.ref, c);

        cross_ld(b, c, u); grad.at(f[0]) += vec3d((double)u[0], (double)u[1], (double)u[2]) / 6.0;
        cross_ld(c, a, u); grad.at(f[1]) += vec3d((double)u[0], (double)u[1], (double)u[2]) / 6.0;
        cross_ld(a, b, u); grad.at(f[2]) += vec3d((double)u[0], (double)u[1], (double)u[2]) / 6.0;
    }
}

//::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::

// E(x) = sum over undirected mesh edges of ||x_i - x_j||^2
CINO_INLINE
long double edge_energy(const std::vector<vec3d> & coords,
                        const std::vector<uint>  & edges)
{
    long double energy = 0.0L;
    const uint num_edges = uint(edges.size()/2);
    for(uint eid=0; eid<num_edges; ++eid)
    {
        const uint i = edges.at(2*eid    );
        const uint j = edges.at(2*eid + 1);
        const long double dx = (long double)coords.at(i).x() - (long double)coords.at(j).x();
        const long double dy = (long double)coords.at(i).y() - (long double)coords.at(j).y();
        const long double dz = (long double)coords.at(i).z() - (long double)coords.at(j).z();
        energy += dx*dx + dy*dy + dz*dz;
    }
    return energy;
}

//::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::

// oriented area vector of triangle (a,b,c); twice the triangle area as a
// vector whose direction is the face normal
CINO_INLINE
vec3d area_vector(const vec3d & a, const vec3d & b, const vec3d & c)
{
    const vec3d ab(b.x()-a.x(), b.y()-a.y(), b.z()-a.z());
    const vec3d ac(c.x()-a.x(), c.y()-a.y(), c.z()-a.z());
    return vec3d(ab.y()*ac.z()-ab.z()*ac.y(),
                 ab.z()*ac.x()-ab.x()*ac.z(),
                 ab.x()*ac.y()-ab.y()*ac.x());
}

CINO_INLINE
bool is_degenerate_triangle(const vec3d & a, const vec3d & b, const vec3d & c)
{
    const vec3d ab(b.x()-a.x(), b.y()-a.y(), b.z()-a.z());
    const vec3d ac(c.x()-a.x(), c.y()-a.y(), c.z()-a.z());
    const vec3d n = area_vector(a,b,c);
    if(!finite_vec(n)) return true;

    long double scale_sq = (long double)ab.dot(ab);
    const long double ac_sq = (long double)ac.dot(ac);
    const vec3d bc(c.x()-b.x(), c.y()-b.y(), c.z()-b.z());
    const long double bc_sq = (long double)bc.dot(bc);
    scale_sq = std::max(scale_sq, std::max(ac_sq, bc_sq));
    if(scale_sq <= 0.0L) return true;

    // The cross product length is twice the triangle area. This relative
    // threshold only rejects numerically collinear triangles: legitimately
    // thin (but non degenerate) triangles are accepted and can be used to
    // exercise the no-flip guard.
    const long double n_sq = (long double)n.dot(n);
    return n_sq <= 1e-24L * scale_sq * scale_sq;
}

} // namespace volume_preserving_fairing_detail

//::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
//                               PUBLIC API
//::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::

CINO_INLINE
const char * volume_preserving_fairing_stop_reason_str(const VolumePreservingFairingStopReason reason)
{
    switch(reason)
    {
        case VolumePreservingFairingStopReason::MAX_ITERATIONS  : return "max_iterations";
        case VolumePreservingFairingStopReason::STATIONARY      : return "stationary";
        case VolumePreservingFairingStopReason::NO_FREE_VERTICES: return "no_free_vertices";
        case VolumePreservingFairingStopReason::NO_LEGAL_STEP   : return "no_legal_step";
        default                                                 : return "unknown";
    }
}

//::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::

template<class M, class V, class E, class P>
CINO_INLINE
VolumePreservingFairingReport volume_preserving_fairing(      Trimesh<M,V,E,P> & m,
                                                         const std::vector<uint> & fixed_vertices,
                                                         const VolumePreservingFairingOptions & options)
{
    using namespace volume_preserving_fairing_detail;

    //------------------------------------------------------------------
    // OPTIONS VALIDATION (before any coordinate is touched)
    //------------------------------------------------------------------
    if(!std::isfinite(options.step) || options.step <= 0.0 || options.step > 1.0)
    {
        fail("invalid options: step must lie in (0,1]");
    }
    if(!std::isfinite(options.relative_volume_tolerance) ||
        options.relative_volume_tolerance <= 0.0)
    {
        fail("invalid options: relative_volume_tolerance must be positive and finite");
    }

    //------------------------------------------------------------------
    // INPUT VALIDATION (read only: no coordinate is rewritten on failure)
    //------------------------------------------------------------------
    const uint nv = m.num_verts();
    const uint ne = m.num_edges();
    const uint np = m.num_polys();

    for(uint vid=0; vid<nv; ++vid)
    {
        if(!finite_vec(m.vert(vid))) fail("non finite vertex coordinates");
    }

    std::vector<char> fixed(nv, 0);
    for(uint vid : fixed_vertices)
    {
        if(vid >= nv) fail("fixed vertex id out of range");
        fixed[vid] = 1; // duplicate ids are silently ignored
    }

    // isolated vertices have no incident polygon: they are preserved anyway
    for(uint vid=0; vid<nv; ++vid)
    {
        if(m.adj_v2p(vid).empty()) fixed[vid] = 1;
    }

    // take private copies: the mesh coordinates will be rewritten at the
    // end, and no internal computation must alias them
    const std::vector<vec3d>             coords0 = m.vector_verts();
    const std::vector<std::vector<uint>> polys   = m.vector_polys();
    const std::vector<uint>              edges   = m.vector_edges();

    // faces must be valid triangles
    std::vector<vec3d> initial_area_vec(np, vec3d(0,0,0));
    for(uint pid=0; pid<np; ++pid)
    {
        const std::vector<uint> & f = polys.at(pid);
        if(f.size() != 3) fail("non triangular polygon");
        for(uint k=0; k<3; ++k)
        {
            if(f[k] >= nv) fail("polygon references an out of range vertex");
            for(uint l=k+1; l<3; ++l)
            {
                if(f[k] == f[l]) fail("degenerate triangle (repeated vertex)");
            }
        }
        const vec3d a = coords0.at(f[0]);
        const vec3d b = coords0.at(f[1]);
        const vec3d c = coords0.at(f[2]);
        if(is_degenerate_triangle(a,b,c)) fail("degenerate triangle in input mesh");
        const vec3d av = area_vector(a,b,c);
        if(!finite_vec(av)) fail("non finite triangle area");
        initial_area_vec[pid] = av;
    }

    // closed, non-manifold-free and consistently oriented surface
    for(uint eid=0; eid<ne; ++eid)
    {
        const std::vector<uint> & incident = m.adj_e2p(eid);
        if(incident.size() != 2) fail("mesh is not a closed 2-manifold (edge valence != 2)");
        const uint p0 = incident.at(0);
        const uint p1 = incident.at(1);
        // the two faces must traverse the shared edge in opposite directions
        if(m.edge_is_CCW(eid,p0) == m.edge_is_CCW(eid,p1))
        {
            fail("inconsistent face orientation");
        }
    }
    for(uint vid=0; vid<nv; ++vid)
    {
        if(!m.adj_v2p(vid).empty() && !m.vert_is_manifold(vid))
        {
            fail("non manifold vertex");
        }
    }

    //------------------------------------------------------------------
    // SHELL DECOMPOSITION (faces connected through shared edges)
    //------------------------------------------------------------------
    std::vector<uint> face_shell(np, std::numeric_limits<uint>::max());
    std::vector<Shell> shells;
    for(uint seed=0; seed<np; ++seed)
    {
        if(face_shell.at(seed) != std::numeric_limits<uint>::max()) continue;
        Shell shell;
        shell.min_vid = std::numeric_limits<uint>::max();
        std::queue<uint> front;
        const uint sid = uint(shells.size());
        front.push(seed);
        face_shell.at(seed) = sid;
        while(!front.empty())
        {
            const uint pid = front.front();
            front.pop();
            shell.faces.push_back(pid);
            for(uint vid : polys.at(pid))
            {
                shell.min_vid = std::min(shell.min_vid, vid);
                shell.verts.push_back(vid);
            }
            for(uint nbr : m.adj_p2p(pid))
            {
                if(face_shell.at(nbr) == std::numeric_limits<uint>::max())
                {
                    face_shell.at(nbr) = sid;
                    front.push(nbr);
                }
            }
        }
        std::sort(shell.verts.begin(), shell.verts.end());
        shell.verts.erase(std::unique(shell.verts.begin(), shell.verts.end()), shell.verts.end());
        shells.push_back(std::move(shell));
    }

    // shells are numbered by increasing minimum vertex id
    std::sort(shells.begin(), shells.end(),
              [](const Shell & a, const Shell & b){ return a.min_vid < b.min_vid; });

    std::vector<uint> shell_of_vertex(nv, std::numeric_limits<uint>::max());
    for(uint sid=0; sid<shells.size(); ++sid)
    {
        for(uint vid : shells[sid].verts)
        {
            if(shell_of_vertex.at(vid) != std::numeric_limits<uint>::max())
            {
                fail("vertex shared by two shells (non manifold input)");
            }
            shell_of_vertex.at(vid) = sid;
        }
    }

    // shell-local reference point (centroid), bbox diagonal, initial volume
    for(Shell & shell : shells)
    {
        long double sx=0.0L, sy=0.0L, sz=0.0L;
        long double bb_min[3] = {  std::numeric_limits<long double>::max(),
                                   std::numeric_limits<long double>::max(),
                                   std::numeric_limits<long double>::max()};
        long double bb_max[3] = { -std::numeric_limits<long double>::max(),
                                  -std::numeric_limits<long double>::max(),
                                  -std::numeric_limits<long double>::max()};
        for(uint vid : shell.verts)
        {
            const long double x = (long double)coords0.at(vid).x();
            const long double y = (long double)coords0.at(vid).y();
            const long double z = (long double)coords0.at(vid).z();
            sx += x; sy += y; sz += z;
            bb_min[0] = std::min(bb_min[0], x); bb_max[0] = std::max(bb_max[0], x);
            bb_min[1] = std::min(bb_min[1], y); bb_max[1] = std::max(bb_max[1], y);
            bb_min[2] = std::min(bb_min[2], z); bb_max[2] = std::max(bb_max[2], z);
        }
        const long double nverts = (long double)shell.verts.size();
        shell.ref = vec3d((double)(sx/nverts), (double)(sy/nverts), (double)(sz/nverts));
        const long double dx = bb_max[0]-bb_min[0];
        const long double dy = bb_max[1]-bb_min[1];
        const long double dz = bb_max[2]-bb_min[2];
        shell.diagonal = std::sqrt(dx*dx + dy*dy + dz*dz);
        shell.initial_volume = shell_volume(coords0, polys, shell.faces, shell.ref);
        if(!std::isfinite((double)shell.initial_volume)) fail("non finite shell volume");
        if(std::abs(shell.initial_volume) <= 1e-14L * shell.diagonal*shell.diagonal*shell.diagonal)
        {
            fail("zero volume shell");
        }
    }

    //------------------------------------------------------------------
    // FREE VERTICES AND DEGREES
    //------------------------------------------------------------------
    std::vector<double> degree(nv, 0.0);
    for(uint vid=0; vid<nv; ++vid) degree[vid] = (double)m.adj_v2v(vid).size();

    std::vector<uint> free_verts;
    for(uint vid=0; vid<nv; ++vid)
    {
        if(!fixed.at(vid) && !m.adj_v2p(vid).empty()) free_verts.push_back(vid);
    }

    long double global_diag = 0.0L;
    for(const Shell & shell : shells) global_diag = std::max(global_diag, shell.diagonal);

    //------------------------------------------------------------------
    // REPORT INITIALIZATION
    //------------------------------------------------------------------
    VolumePreservingFairingReport report;
    report.initial_energy = (double)edge_energy(coords0, edges);
    report.final_energy   = report.initial_energy;
    report.shells.resize(shells.size());
    for(uint sid=0; sid<shells.size(); ++sid)
    {
        VolumePreservingFairingShell & rs = report.shells.at(sid);
        rs.id             = sid;
        rs.min_vertex_id  = shells[sid].min_vid;
        rs.vertices       = shells[sid].verts;
        rs.initial_volume = (double)shells[sid].initial_volume;
        rs.final_volume   = rs.initial_volume;
    }
    report.stop = VolumePreservingFairingStopReason::MAX_ITERATIONS;

    if(free_verts.empty())
    {
        report.stop = VolumePreservingFairingStopReason::NO_FREE_VERTICES;
        report.stop_reason = volume_preserving_fairing_stop_reason_str(report.stop);
        report.stop_message = "every vertex is fixed (or the mesh has no triangles)";
        report.completed_steps = 0;
        // nothing moved, but on a successful return the derived data must be
        // consistent with the (unchanged) final coordinates
        m.update_normals();
        m.update_bbox();
        return report;
    }

    // Scratch buffers. `probe` holds tentative coordinates, so an attempt
    // that is not accepted never leaks into the mesh or into any cache.
    std::vector<vec3d> coords     = coords0;
    std::vector<vec3d> probe      = coords0;
    std::vector<vec3d> d          (nv, vec3d(0,0,0));
    std::vector<vec3d> grad       (nv, vec3d(0,0,0));
    std::vector<vec3d> corr       (nv, vec3d(0,0,0)); // h_i = grad_i / deg_i
    std::vector<vec3d> grad_probe (nv, vec3d(0,0,0));

    struct ShellSolve
    {
        long double k      = 0.0L; // sum_i g_i . d_i          (dV slope)
        long double denom  = 0.0L; // sum_i ||g_i||^2 / deg_i   (correction metric)
        long double lambda = 0.0L; // current volume correction multiplier
    };

    const uint   max_backtrack        = 48;
    const double volume_target_factor = 0.5; // aim below the requested tolerance

    for(uint iteration=1; iteration<=options.max_iterations; ++iteration)
    {
        report.attempted_steps = iteration;

        //--------------------------------------------------------------
        // descent direction for the edge energy (uniform Laplacian):
        //   grad_i E = 2*deg_i*(x_i - centroid of neighbours)
        //   d_i      = centroid(neighbours) - x_i = -grad_i E / (2*deg_i)
        //--------------------------------------------------------------
        long double max_d = 0.0L;
        for(uint vid : free_verts)
        {
            const std::vector<uint> & nbrs = m.adj_v2v(vid);
            const long double deg = (long double)nbrs.size();
            long double sx=0.0L, sy=0.0L, sz=0.0L;
            for(uint nbr : nbrs)
            {
                sx += (long double)coords.at(nbr).x();
                sy += (long double)coords.at(nbr).y();
                sz += (long double)coords.at(nbr).z();
            }
            const long double dx = sx/deg - (long double)coords.at(vid).x();
            const long double dy = sy/deg - (long double)coords.at(vid).y();
            const long double dz = sz/deg - (long double)coords.at(vid).z();
            d.at(vid) = vec3d((double)dx, (double)dy, (double)dz);
            max_d = std::max(max_d, std::sqrt(dx*dx + dy*dy + dz*dz));
        }

        //--------------------------------------------------------------
        // per-shell volume gradient and coupled first order correction.
        // The fairing field d_i changes every shell volume at rate
        //   k = sum_i g_i . d_i
        // and the correction field h_i = g_i / deg_i changes it at rate
        //   denom = sum_i ||g_i||^2 / deg_i
        // so lambda = t*k/denom removes that change to first order, inside
        // the very same step (never as a separate final rescaling).
        //--------------------------------------------------------------
        std::vector<ShellSolve> solve(shells.size());
        long double metric = 0.0L; // slope of E along d     (sum deg ||d||^2)
        long double proj   = 0.0L; // slope absorbed by volume correction
        for(uint vid : free_verts)
        {
            const long double deg = (long double)degree.at(vid);
            metric += deg * ((long double)d.at(vid).dot(d.at(vid)));
        }
        for(uint sid=0; sid<shells.size(); ++sid)
        {
            const Shell & shell = shells.at(sid);
            shell_volume_gradient(coords, polys, shell, grad);
            for(uint vid : shell.verts)
            {
                if(fixed.at(vid)) continue;
                const long double deg = (long double)degree.at(vid);
                const vec3d & g    = grad.at(vid);
                const vec3d & dvec = d.at(vid);
                solve[sid].k     += (long double)g.dot(dvec);
                solve[sid].denom += ((long double)g.dot(g)) / deg;
            }
            solve[sid].lambda = (solve[sid].denom > 0.0L)
                              ? (solve[sid].k / solve[sid].denom)
                              : 0.0L;
            if(solve[sid].denom > 0.0L)
            {
                proj += (solve[sid].k * solve[sid].k) / solve[sid].denom;
            }
        }
        // correction field h_i = grad_i / deg_i, cached once per iteration
        for(uint sid=0; sid<shells.size(); ++sid)
        {
            const Shell & shell = shells.at(sid);
            for(uint vid : shell.verts)
            {
                if(fixed.at(vid)) continue;
                const long double deg = (long double)degree.at(vid);
                corr.at(vid) = grad.at(vid) / (double)deg;
            }
        }

        //--------------------------------------------------------------
        // stopping criteria evaluated *before* attempting any step.
        // `metric - proj` is the slope of E along the volume-constrained
        // direction; when it vanishes (or the Laplacian direction is zero)
        // there is no first order descent, which is the case of already
        // stationary / symmetric configurations.
        //--------------------------------------------------------------
        const long double d_eps = 1e-12L * std::max(global_diag, (long double)1e-300);
        if(max_d <= d_eps)
        {
            report.stop = VolumePreservingFairingStopReason::STATIONARY;
            report.stop_reason = volume_preserving_fairing_stop_reason_str(report.stop);
            report.stop_message = "uniform Laplacian descent direction is (numerically) zero";
            break;
        }
        if(metric <= 0.0L || (metric - proj) <= 1e-12L * metric)
        {
            report.stop = VolumePreservingFairingStopReason::STATIONARY;
            report.stop_reason = volume_preserving_fairing_stop_reason_str(report.stop);
            report.stop_message = "no first order descent left along the constrained direction";
            break;
        }

        //--------------------------------------------------------------
        // backtracking on the step length. For every trial the exact (non
        // linear) per-shell volume constraint is solved with a scalar
        // Newton iteration on lambda.
        //--------------------------------------------------------------
        const long double energy_before = edge_energy(coords, edges);
        bool accepted = false;
        uint attempts = 0;
        double accepted_step = 0.0;
        long double energy_after = energy_before;
        double residual_after = 0.0;

        // x_i(t,lambda) = x_i + t*d_i - lambda*g_i/deg_i. The direction
        // g_i is held fixed during the scalar Newton solve; the derivative
        // is evaluated on the exact trial geometry, so the solved lambda
        // is the true (not linearized) correction.
        auto apply_displacement = [&](const Shell & shell, double tt, long double lambda)
        {
            for(uint vid : shell.verts)
            {
                if(fixed.at(vid)) continue;
                const long double px = (long double)coords.at(vid).x() + (long double)tt*(long double)d.at(vid).x() - lambda*(long double)corr.at(vid).x();
                const long double py = (long double)coords.at(vid).y() + (long double)tt*(long double)d.at(vid).y() - lambda*(long double)corr.at(vid).y();
                const long double pz = (long double)coords.at(vid).z() + (long double)tt*(long double)d.at(vid).z() - lambda*(long double)corr.at(vid).z();
                probe.at(vid) = vec3d((double)px, (double)py, (double)pz);
            }
        };

        double t = options.step;
        for(uint backtrack=0; backtrack<=max_backtrack; ++backtrack, t*=0.5)
        {
            ++attempts;

            // every shell owns a disjoint vertex set, so per-shell solves compose
            probe = coords;
            for(uint sid=0; sid<shells.size(); ++sid)
            {
                const Shell      & shell = shells.at(sid);
                ShellSolve       & s     = solve.at(sid);

                // first order guess
                s.lambda = (s.denom > 0.0L) ? ((long double)t * s.k / s.denom) : 0.0L;
                if(s.denom <= 0.0L)
                {
                    // the shell volume does not depend on its free vertices
                    // (to first order): lambda = 0 is the right correction
                    apply_displacement(shell, t, 0.0L);
                    continue;
                }

                const long double v0     = shell.initial_volume;
                const long double target = volume_target_factor *
                                           options.relative_volume_tolerance *
                                           std::abs(v0);
                long double best_lambda = s.lambda;
                long double best_res    = std::numeric_limits<long double>::max();

                for(uint nit=0; nit<80; ++nit)
                {
                    apply_displacement(shell, t, s.lambda);
                    const long double v    = shell_volume(probe, polys, shell.faces, shell.ref);
                    const long double res  = v - v0;
                    const long double ares = std::abs(res);
                    if(ares < best_res)
                    {
                        best_res    = ares;
                        best_lambda = s.lambda;
                    }
                    if(ares <= target) break;

                    // dV/dlambda = -sum_i h_i . grad_i(probe), where h_i is the
                    // fixed correction direction (corr) and grad_i(probe) is
                    // the exact gradient at the trial configuration
                    shell_volume_gradient(probe, polys, shell, grad_probe);
                    long double der = 0.0L;
                    for(uint vid : shell.verts)
                    {
                        if(fixed.at(vid)) continue;
                        der -= (long double)corr.at(vid).dot(grad_probe.at(vid));
                    }
                    if(!std::isfinite((double)der) || std::abs(der) <= 1e-300L) break;
                    const long double next = s.lambda - res/der;
                    if(!std::isfinite((double)next)) break;
                    s.lambda = next;
                }

                // use the best lambda found; the acceptance stage below
                // measures the exact residual and the requested tolerance
                s.lambda = best_lambda;
                apply_displacement(shell, t, s.lambda);
            }

            //-------------------- acceptance test --------------------
            bool valid = true;
            for(uint vid : free_verts)
            {
                if(!finite_vec(probe.at(vid))) { valid = false; break; }
            }
            if(!valid) continue;

            const long double energy_trial = edge_energy(probe, edges);
            if(!(energy_trial < energy_before)) continue;

            double worst_residual = 0.0;
            bool volumes_ok = true;
            for(const Shell & shell : shells)
            {
                const long double v = shell_volume(probe, polys, shell.faces, shell.ref);
                const long double den = std::max(std::abs(shell.initial_volume), (long double)1e-300);
                const double rel = (double)(std::abs(v - shell.initial_volume) / den);
                worst_residual = std::max(worst_residual, rel);
                if(!(rel <= options.relative_volume_tolerance))
                {
                    volumes_ok = false;
                    break;
                }
            }
            if(!volumes_ok) continue;

            // oriented area checks on every face, against the *initial*
            // geometry: no face may flip its normal, and no face may be
            // squashed below 1e-6 of its original area. This prevents a
            // volume compensation from turning thin triangles inside out.
            bool faces_ok = true;
            for(uint pid=0; pid<np; ++pid)
            {
                const std::vector<uint> & f = polys.at(pid);
                const vec3d av = area_vector(probe.at(f[0]), probe.at(f[1]), probe.at(f[2]));
                if(!finite_vec(av)) { faces_ok = false; break; }
                if((long double)initial_area_vec.at(pid).dot(av) <= 0.0L)
                {
                    faces_ok = false; break; // normal flipped
                }
                const long double len_new = std::sqrt((long double)av.dot(av));
                const long double len_old = std::sqrt((long double)initial_area_vec.at(pid).dot(initial_area_vec.at(pid)));
                if(!(len_new >= 1e-6L * len_old))
                {
                    faces_ok = false; break; // face squashed
                }
            }
            if(!faces_ok) continue;

            // accepted
            coords         = probe;
            accepted       = true;
            accepted_step  = t;
            energy_after   = energy_trial;
            residual_after = worst_residual;
            break;
        }

        if(!accepted)
        {
            report.stop = VolumePreservingFairingStopReason::NO_LEGAL_STEP;
            report.stop_reason = volume_preserving_fairing_stop_reason_str(report.stop);
            report.stop_message = "no step satisfied the volume, face-validity and energy-descent constraints";
            break;
        }

        VolumePreservingFairingStep step;
        step.iteration           = iteration;
        step.step_index          = report.accepted_steps + 1;
        step.attempts            = attempts;
        step.step_size           = accepted_step;
        step.energy_before       = (double)energy_before;
        step.energy              = (double)energy_after;
        step.constraint_residual = residual_after;
        step.volume_residual     = residual_after;
        report.steps.push_back(step);
        report.energies.push_back((double)energy_after);
        report.accepted_steps  = uint(report.steps.size());
        report.completed_steps = report.accepted_steps;
        report.final_energy    = (double)energy_after;
    }

    // stop reason not set yet => the loop exhausted its iterations
    if(report.stop_reason.empty())
    {
        report.stop = VolumePreservingFairingStopReason::MAX_ITERATIONS;
        report.stop_reason = volume_preserving_fairing_stop_reason_str(report.stop);
        report.stop_message = "maximum number of accepted steps reached";
    }
    report.completed_steps = report.accepted_steps;

    //------------------------------------------------------------------
    // COMMIT: only now are the mesh coordinates rewritten. Fixed vertices
    // are not assigned at all, so their values are bitwise untouched.
    // Connectivity and all custom per-element attributes are not touched.
    // A run with zero accepted steps does not assign anything either: no
    // rejected attempt may leave a trace on the mesh.
    //------------------------------------------------------------------
    if(report.accepted_steps > 0)
    {
        for(uint vid : free_verts)
        {
            m.vert(vid) = coords.at(vid);
        }
    }

    // make derived data consistent with the final coordinates. This is done
    // unconditionally on the normal path (even when no step was accepted):
    // it is the only guarantee the caller can rely on, and it never rewrites
    // coordinates.
    m.update_normals();
    m.update_bbox();

    // final per-shell signed volumes, each measured with its own local
    // reference point (the same gauge used throughout)
    for(uint sid=0; sid<shells.size(); ++sid)
    {
        const long double v = shell_volume(coords, polys, shells[sid].faces, shells[sid].ref);
        report.shells.at(sid).final_volume = (double)v;
    }
    report.final_energy = (double)edge_energy(coords, edges);

    return report;
}

//::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::

template<class M, class V, class E, class P>
CINO_INLINE
VolumePreservingFairingReport volume_preserving_fairing(      Trimesh<M,V,E,P> & m,
                                                         const std::set<uint>    & fixed_vertices,
                                                         const VolumePreservingFairingOptions & options)
{
    return volume_preserving_fairing(m, std::vector<uint>(fixed_vertices.begin(), fixed_vertices.end()), options);
}

//::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::

template<class M, class V, class E, class P>
CINO_INLINE
VolumePreservingFairingReport volume_preserving_fairing(      Trimesh<M,V,E,P> & m,
                                                         const std::unordered_set<uint> & fixed_vertices,
                                                         const VolumePreservingFairingOptions & options)
{
    return volume_preserving_fairing(m, std::vector<uint>(fixed_vertices.begin(), fixed_vertices.end()), options);
}

} // namespace cinolib
