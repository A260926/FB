/********************************************************************************
*  This file is part of CinoLib                                                 *
*  Copyright(C) 2024: CinoLib contributors                                      *
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
*********************************************************************************/
#include <cinolib/surface_correspondence.h>

#include <Eigen/Dense>

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>


namespace cinolib
{

namespace
{

constexpr double BARY_EPS   = 1e-12; // barycentric sum tolerance (contract)
constexpr double PARAM_EPS  = 1e-10; // point in triangle tolerance in the disks
constexpr double CLASS_EPS  = 1e-10; // slack for points on shared clipping edges
constexpr double AREA_EPS2D = 1e-13; // minimum positive signed area in the unit disk
constexpr double COVER_TOL  = 1e-9;  // relative area conservation tolerance

const double PI = std::acos(-1.0);

using Bary3 = std::array<double,3>;
using Tri3  = std::array<uint,3>;

//::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::

bool finite_vec(const vec3d & p)
{
    return std::isfinite(p.x()) && std::isfinite(p.y()) && std::isfinite(p.z());
}

bool valid_bary(const Bary3 & b)
{
    for(double w : b) if(!std::isfinite(w) || w < -BARY_EPS) return false;
    return std::fabs(b[0] + b[1] + b[2] - 1.0) <= BARY_EPS;
}

Bary3 snap_bary(Bary3 b)
{
    // clamp small negative weights created by points on shared arrangement
    // edges, then renormalize so the result lies exactly on the simplex
    for(double & w : b) if(w < 0.0 && w > -CLASS_EPS) w = 0.0;
    double s = b[0] + b[1] + b[2];
    if(s > 0.0) for(double & w : b) w /= s;
    return b;
}

double signed_area2d(const vec2d & a, const vec2d & b, const vec2d & c)
{
    return 0.5 * ((b.x()-a.x()) * (c.y()-a.y()) - (b.y()-a.y()) * (c.x()-a.x()));
}

Bary3 bary2d(const std::array<vec2d,3> & t, const vec2d & p)
{
    double den = (t[1].y()-t[2].y()) * (t[0].x()-t[2].x())
               + (t[2].x()-t[1].x()) * (t[0].y()-t[2].y());
    Bary3 w{0.0,0.0,0.0};
    if(std::fabs(den) < std::numeric_limits<double>::min()) return w;
    w[0] = ((t[1].y()-t[2].y()) * (p.x()-t[2].x())
          + (t[2].x()-t[1].x()) * (p.y()-t[2].y())) / den;
    w[1] = ((t[2].y()-t[0].y()) * (p.x()-t[2].x())
          + (t[0].x()-t[2].x()) * (p.y()-t[2].y())) / den;
    w[2] = 1.0 - w[0] - w[1];
    return w;
}

bool inside2d(const Bary3 & w)
{
    return w[0] >= -PARAM_EPS && w[1] >= -PARAM_EPS && w[2] >= -PARAM_EPS;
}

vec3d eval3d(const std::array<vec3d,3> & t, const Bary3 & w)
{
    return w[0]*t[0] + w[1]*t[1] + w[2]*t[2];
}

double tri3_area(const std::array<vec3d,3> & t)
{
    return 0.5 * (t[1]-t[0]).cross(t[2]-t[0]).norm();
}

uint64_t edge_key(uint a, uint b)
{
    if(a > b) std::swap(a,b);
    return (uint64_t(a) << 32) | uint64_t(b);
}

//::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
// A planar triangulated disk parameterized inside the unit circle.
// Vertices carry global mesh ids (or, for the synthetic post disk, the ids
// of the mesh vertices they represent).
//::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::

struct PlanarDisk
{
    std::vector<uint>  verts;    // global ids, indexed by local id
    std::vector<Tri3>  tris;     // triangles, local ids, CCW in (u,v)
    std::vector<uint>  boundary; // ordered boundary loop, local ids
    std::vector<vec2d> uv;       // planar position per local vertex

    int local_id(uint gid) const
    {
        for(uint i=0;i<verts.size();++i) if(verts[i]==gid) return int(i);
        return -1;
    }
};

// Flip-free planar parameterization of a triangle soup disk. The boundary
// loop is pinned on the unit circle, proportionally to cumulative 3D edge
// length in the given loop order; interior vertices solve a uniform weight
// harmonic system (Tutte embedding).
bool parameterize_soup(const std::vector<Tri3> & soup,
                       const std::vector<uint> & loop,
                       const std::function<vec3d(uint)> & position,
                       PlanarDisk & disk)
{
    disk = PlanarDisk();
    if(soup.empty() || loop.size() < 3) return false;

    std::unordered_map<uint,uint> local;
    auto lid = [&](uint vid) -> uint
    {
        auto it = local.find(vid);
        if(it != local.end()) return it->second;
        uint id = uint(disk.verts.size());
        local[vid] = id;
        disk.verts.push_back(vid);
        disk.uv.push_back(vec2d(0,0));
        return id;
    };

    std::unordered_set<uint> bnd_set(loop.begin(), loop.end());
    for(uint vid : loop) lid(vid);
    for(const Tri3 & g : soup)
    {
        Tri3 t{lid(g[0]), lid(g[1]), lid(g[2])};
        disk.tris.push_back(t);
    }
    uint n = uint(disk.verts.size());
    disk.boundary.reserve(loop.size());
    for(uint vid : loop) disk.boundary.push_back(local.at(vid));

    // boundary positions on the unit circle, arc length = 3D edge proportion
    double perim = 0.0;
    for(size_t i=0;i<loop.size();++i)
        perim += position(loop[i]).dist(position(loop[(i+1)%loop.size()]));
    if(!(perim > 0.0)) return false;

    double acc = 0.0;
    for(size_t i=0;i<loop.size();++i)
    {
        double ang = 2.0 * PI * acc / perim;
        disk.uv[disk.boundary[i]] = vec2d(std::cos(ang), std::sin(ang));
        acc += position(loop[i]).dist(position(loop[(i+1)%loop.size()]));
    }

    // uniform harmonic positions for interior vertices (reduced symmetric system)
    std::vector<uint> interior;
    for(uint i=0;i<n;++i) if(!bnd_set.count(disk.verts[i])) interior.push_back(i);
    if(!interior.empty())
    {
        std::unordered_map<uint,uint> idx;
        for(uint k=0;k<interior.size();++k) idx[interior[k]] = k;
        std::vector<std::vector<uint>> nbrs(n);
        static const int ee[3][2] = {{0,1},{1,2},{2,0}};
        for(const Tri3 & t : disk.tris)
            for(int k=0;k<3;++k)
            {
                uint a = t[ee[k][0]], b = t[ee[k][1]];
                nbrs[a].push_back(b);
                nbrs[b].push_back(a);
            }
        for(auto & nb : nbrs)
        {
            std::sort(nb.begin(), nb.end());
            nb.erase(std::unique(nb.begin(), nb.end()), nb.end());
        }
        uint ni = uint(interior.size());
        Eigen::MatrixXd A = Eigen::MatrixXd::Zero(ni,ni);
        Eigen::MatrixXd rhs = Eigen::MatrixXd::Zero(ni,2);
        for(uint k=0;k<ni;++k)
        {
            uint i = interior[k];
            A(k,k) = double(nbrs[i].size());
            for(uint j : nbrs[i])
            {
                if(idx.count(j)) A(k, idx[j]) = -1.0;
                else
                {
                    rhs(k,0) += disk.uv[j].x();
                    rhs(k,1) += disk.uv[j].y();
                }
            }
        }
        Eigen::LDLT<Eigen::MatrixXd> solver(A);
        if(solver.info() != Eigen::Success) return false;
        Eigen::MatrixXd sol = solver.solve(rhs);
        if(!sol.allFinite()) return false;
        for(uint k=0;k<ni;++k) disk.uv[interior[k]] = vec2d(sol(k,0), sol(k,1));
    }

    // strict flip/degeneracy rejection of the whole embedding
    for(const Tri3 & t : disk.tris)
        if(signed_area2d(disk.uv[t[0]], disk.uv[t[1]], disk.uv[t[2]]) <= AREA_EPS2D) return false;
    return true;
}


//::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
// Planar triangle on the source side of the correspondence: the image of
// an existing correspondence patch inside the pre-collapse planar disk.
//::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::

struct SourcePlanarTri
{
    uint sf = 0;                 // immutable source face
    std::array<vec2d,3> uv{vec2d(0,0), vec2d(0,0), vec2d(0,0)}; // planar corners
    // source-face barycentric coords of each planar corner (composition chain)
    std::array<Bary3,3> bary{Bary3{0,0,0},Bary3{0,0,0},Bary3{0,0,0}};
};

struct CurrentPlanarTri
{
    uint oldFace = 0;            // pre-collapse current face id
    uint newFace = 0;            // post-collapse dense face id (filled in later)
    std::array<vec2d,3> uv{vec2d(0,0), vec2d(0,0), vec2d(0,0)}; // post-disk corners
};

double cross2(const vec2d & a, const vec2d & b) { return a.x()*b.y() - a.y()*b.x(); }

// Sutherland-Hodgman clipping of a convex polygon by a CCW triangle
std::vector<vec2d> clip_polygon_triangle(const std::vector<vec2d> & subj,
                                         const std::array<vec2d,3> & clip)
{
    const double tol = 1e-12;
    std::vector<vec2d> out = subj;
    for(int e=0;e<3 && !out.empty();++e)
    {
        const vec2d & A = clip[e];
        const vec2d & B = clip[(e+1)%3];
        auto side = [&](const vec2d & p){ return cross2(B-A, p-A); };
        std::vector<vec2d> in; in.swap(out);
        for(size_t i=0;i<in.size();++i)
        {
            const vec2d & P = in[i];
            const vec2d & Q = in[(i+1)%in.size()];
            bool pin = side(P) >= -tol, qin = side(Q) >= -tol;
            auto intersect = [&](){
                double den = cross2(B-A, P-Q);
                double t = (std::fabs(den) > std::numeric_limits<double>::min())
                         ? cross2(B-A, P-A)/den : 0.0;
                t = std::max(0.0, std::min(1.0, t));
                return P + t*(Q-P);
            };
            if(pin && qin)       out.push_back(Q);
            else if(pin && !qin) out.push_back(intersect());
            else if(!pin && qin){ out.push_back(intersect()); out.push_back(Q); }
        }
        std::vector<vec2d> dedup;
        for(const vec2d & p : out)
            if(dedup.empty() || (p-dedup.back()).norm() > 1e-12) dedup.push_back(p);
        if(dedup.size()>1 && (dedup.front()-dedup.back()).norm() <= 1e-12) dedup.pop_back();
        out.swap(dedup);
    }
    return out;
}

} // anonymous namespace

//::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::

struct SurfaceCorrespondence::Impl
{
    Trimesh<> mesh;                          // independent working copy
    std::vector<vec3d> srcVerts;             // immutable source coordinates
    std::vector<Tri3>  srcFaces;             // immutable source triangles
    std::unordered_map<uint64_t,std::vector<uint>> srcEdgeFaces; // immutable source adjacency
    std::vector<std::vector<uint>> srcVertFaces;                // immutable source vertex stars
    std::vector<CorrespondencePatch> patches;
    std::vector<std::vector<uint>> srcIndex; // source face -> patch ids
    std::vector<std::vector<uint>> curIndex; // current face -> patch ids
    CollapseRewriteReport lastReport;

    //------------------------------------------------------------------

    Impl(const Trimesh<> & source)
    {
        if(source.num_polys() == 0 || source.num_verts() == 0)
            throw std::invalid_argument("SurfaceCorrespondence: source mesh is empty");

        srcVerts = source.vector_verts();
        srcFaces.reserve(source.num_polys());
        for(uint pid=0; pid<source.num_polys(); ++pid)
        {
            auto vl = source.poly_verts_id(pid);
            if(vl.size() != 3)
                throw std::invalid_argument("SurfaceCorrespondence: source is not a triangle mesh");
            for(uint vid : vl)
                if(vid >= srcVerts.size() || !finite_vec(srcVerts.at(vid)))
                    throw std::invalid_argument("SurfaceCorrespondence: non finite vertex coordinate");
            Tri3 t{vl[0], vl[1], vl[2]};
            if(tri3_area({srcVerts[t[0]], srcVerts[t[1]], srcVerts[t[2]]}) <= 0.0)
                throw std::invalid_argument("SurfaceCorrespondence: degenerate source triangle");
            srcFaces.push_back(t);
        }

        for(uint eid=0; eid<source.num_edges(); ++eid)
        {
            if(source.edge_valence(eid) > 2)
                throw std::invalid_argument("SurfaceCorrespondence: non manifold edge");
            if(source.edge_valence(eid) != 2) continue;
            uint a = source.edge_vert_id(eid,0);
            uint b = source.edge_vert_id(eid,1);
            auto winding = [&](uint pid) -> int
            {
                auto vl = source.poly_verts_id(pid);
                for(int k=0;k<3;++k)
                {
                    if(vl[k]==a && vl[(k+1)%3]==b) return +1;
                    if(vl[k]==b && vl[(k+1)%3]==a) return -1;
                }
                return 0;
            };
            const auto & inc = source.adj_e2p(eid);
            int w0 = winding(inc[0]), w1 = winding(inc[1]);
            if(w0 == 0 || w0 == w1)
                throw std::invalid_argument("SurfaceCorrespondence: inconsistent face orientation");
        }

        std::vector<uint> serialized;
        srcVertFaces.resize(srcVerts.size());
        static const int ee[3][2] = {{0,1},{1,2},{2,0}};
        for(uint fid=0; fid<srcFaces.size(); ++fid)
        {
            const Tri3 & t = srcFaces[fid];
            serialized.insert(serialized.end(), t.begin(), t.end());
            for(int k=0;k<3;++k) srcEdgeFaces[edge_key(t[ee[k][0]], t[ee[k][1]])].push_back(fid);
            for(uint vid : t) srcVertFaces[vid].push_back(fid);
        }
        mesh = Trimesh<>(srcVerts, serialized);

        srcIndex.resize(srcFaces.size());
        curIndex.resize(mesh.num_polys());
        patches.reserve(srcFaces.size());
        for(uint pid=0; pid<srcFaces.size(); ++pid)
        {
            CorrespondencePatch pa;
            pa.sourceFace = pid;
            pa.currentFace = pid;
            pa.sourceBary  = {Bary3{1,0,0}, Bary3{0,1,0}, Bary3{0,0,1}};
            pa.currentBary = {Bary3{1,0,0}, Bary3{0,1,0}, Bary3{0,0,1}};
            uint id = uint(patches.size());
            patches.push_back(pa);
            srcIndex[pid].push_back(id);
            curIndex[pid].push_back(id);
        }
    }

    //------------------------------------------------------------------

    std::array<vec3d,3> source_tri(const uint sf) const
    {
        const Tri3 & t = srcFaces[sf];
        return {srcVerts[t[0]], srcVerts[t[1]], srcVerts[t[2]]};
    }

    std::array<vec3d,3> current_tri(const uint cf) const
    {
        return {mesh.poly_vert(cf,0), mesh.poly_vert(cf,1), mesh.poly_vert(cf,2)};
    }

    static Bary3 bary3d(const std::array<vec3d,3> & t, const vec3d & p)
    {
        vec3d u = t[1]-t[0], v = t[2]-t[0], w = p-t[0];
        double d00 = u.dot(u), d01 = u.dot(v), d11 = v.dot(v);
        double d20 = w.dot(u), d21 = w.dot(v);
        double den = d00*d11 - d01*d01;
        Bary3 b{0.0,0.0,0.0};
        if(std::fabs(den) > std::numeric_limits<double>::min())
        {
            b[2] = (d00*d21 - d01*d20) / den;
            b[1] = (d11*d20 - d01*d21) / den;
            b[0] = 1.0 - b[1] - b[2];
        }
        return b;
    }

    //------------------------------------------------------------------
    // Build the ordered boundary loop of the union of the two endpoint
    // stars. Returns false if the star union is not a triangulated disk.
    //------------------------------------------------------------------
    bool star_boundary_loop(uint v0, uint v1, const std::vector<uint> & star,
                            std::vector<uint> & loop) const
    {
        loop.clear();

        // boundary edges of the star complex: mesh edges with one star face
        std::unordered_map<uint64_t,uint> edge_one_face; // key -> unique star face
        std::set<uint64_t> bnd_edges;
        for(uint pid : star)
            for(uint eid : mesh.adj_p2e(pid))
            {
                uint64_t key = edge_key(mesh.edge_vert_id(eid,0), mesh.edge_vert_id(eid,1));
                if(edge_one_face.count(key)) bnd_edges.erase(key);
                else { edge_one_face[key] = pid; bnd_edges.insert(key); }
            }
        // closed umbrella (e.g. tetrahedron) is not a topological disk
        if(bnd_edges.empty()) return false;

        // adjacency of boundary edges through their shared vertex
        std::unordered_map<uint,std::vector<uint64_t>> vert_edges;
        std::unordered_map<uint64_t,ipair> key_ends;
        for(uint64_t key : bnd_edges)
        {
            uint pid = edge_one_face[key];
            for(uint eid : mesh.adj_p2e(pid))
            {
                uint64_t k2 = edge_key(mesh.edge_vert_id(eid,0), mesh.edge_vert_id(eid,1));
                if(k2 == key)
                {
                    ipair pr(mesh.edge_vert_id(eid,0), mesh.edge_vert_id(eid,1));
                    key_ends[key] = pr;
                    vert_edges[pr.first].push_back(key);
                    vert_edges[pr.second].push_back(key);
                    break;
                }
            }
        }
        for(auto & kv : vert_edges) if(kv.second.size() != 2) return false;

        // orientate: the loop must follow each boundary edge in the direction
        // induced by its incident face, so the star lies on the left (CCW)
        uint64_t start_key = *bnd_edges.begin();
        ipair se = key_ends[start_key];
        uint face = edge_one_face[start_key];
        auto vl = mesh.poly_verts_id(face);
        uint first_v, second_v;
        bool found = false;
        for(int k=0;k<3;++k)
        {
            if(vl[k]==se.first && vl[(k+1)%3]==se.second) { first_v = se.first; second_v = se.second; found=true; break; }
            if(vl[k]==se.second && vl[(k+1)%3]==se.first) { first_v = se.second; second_v = se.first; found=true; break; }
        }
        if(!found) return false;

        loop.push_back(first_v);
        loop.push_back(second_v);
        uint64_t prev_key = start_key;
        uint guard = uint(bnd_edges.size()) + 1;
        while(guard--)
        {
            uint tail = loop.back();
            uint64_t next_key = UINT64_MAX;
            for(uint64_t k : vert_edges[tail]) if(k != prev_key) { next_key = k; break; }
            if(next_key == UINT64_MAX) return false;
            ipair pr = key_ends[next_key];
            uint nv = (pr.first == tail) ? pr.second : pr.first;
            if(nv == loop.front()) break;
            if(std::find(loop.begin(), loop.end(), nv) != loop.end()) return false;
            loop.push_back(nv);
            prev_key = next_key;
        }
        if(loop.size() != bnd_edges.size()) return false;

        // disk checks: only the two contracted endpoints may be interior
        std::unordered_set<uint> on_loop(loop.begin(), loop.end());
        std::unordered_set<uint> star_verts;
        for(uint pid : star) for(uint vid : mesh.poly_verts_id(pid)) star_verts.insert(vid);
        for(uint vid : star_verts)
        {
            if(on_loop.count(vid)) continue;
            if(vid != v0 && vid != v1) return false;
        }
        if(!star_verts.count(v0) || !star_verts.count(v1)) return false;
        // every star triangle must contain an interior vertex (pure fan complex)
        for(uint pid : star)
        {
            auto vv = mesh.poly_verts_id(pid);
            bool has_interior = (vv[0]==v0 || vv[0]==v1 || vv[1]==v0 || vv[1]==v1 || vv[2]==v0 || vv[2]==v1);
            if(!has_interior) return false;
        }
        return true;
    }

    //------------------------------------------------------------------

    bool collapse(uint eid, double lambda)
    {
        if(eid >= mesh.num_edges())
            throw std::invalid_argument("SurfaceCorrespondence::collapse: bad edge id");
        if(!std::isfinite(lambda) || lambda < 0.0 || lambda > 1.0)
            throw std::invalid_argument("SurfaceCorrespondence::collapse: lambda outside [0,1]");
        if(mesh.edge_is_incident_to_boundary(eid)) return false;

        // production admissibility: link condition + geometric checks
        if(!mesh.edge_is_collapsible(eid, lambda)) return false;

        uint v0 = mesh.edge_vert_id(eid,0);
        uint v1 = mesh.edge_vert_id(eid,1);
        uint kept    = std::min(v0,v1);
        uint removed = std::max(v0,v1);
        vec3d kept_new_pos = mesh.edge_sample_at(eid, lambda);

        std::vector<uint> star;
        {
            std::unordered_set<uint> us;
            for(uint pid : mesh.adj_v2p(v0)) us.insert(pid);
            for(uint pid : mesh.adj_v2p(v1)) us.insert(pid);
            star.assign(us.begin(), us.end());
        }
        std::vector<uint> edge_faces = mesh.adj_e2p(eid);
        std::unordered_set<uint> edge_face_set(edge_faces.begin(), edge_faces.end());

        // production collapse replaces every non edge face incident to removed
        // with a fresh face, in the pre-collapse adj_v2p(removed) order. Record
        // old -> added-id pairing here so the rewrite report alone (no rescan)
        // gives the final dense id of every surviving face.
        std::unordered_map<uint,uint> reborn_old_to_add;
        std::vector<uint> removed_star_old;
        {
            for(uint pid : mesh.adj_v2p(removed))
            {
                if(edge_face_set.count(pid)) continue;
                reborn_old_to_add[pid] = UINT_MAX; // filled with poly_adds[k] post collapse
                removed_star_old.push_back(pid);
            }
        }

        std::vector<uint> loop;
        if(!star_boundary_loop(v0, v1, star, loop)) return false;

        // patches attached to the endpoint stars
        std::unordered_set<uint> star_set(star.begin(), star.end());
        std::vector<uint> affected_patches;
        std::unordered_set<uint> affected_src;
        for(uint pid=0; pid<patches.size(); ++pid)
            if(star_set.count(patches[pid].currentFace))
            {
                affected_patches.push_back(pid);
                affected_src.insert(patches[pid].sourceFace);
            }
        if(affected_patches.empty()) return false;

        //-------------- pre-collapse planar disk (real mesh) --------------
        std::vector<Tri3> pre_soup;
        for(uint pid : star)
        {
            auto vv = mesh.poly_verts_id(pid);
            pre_soup.push_back(Tri3{vv[0], vv[1], vv[2]});
        }
        auto pre_pos = [&](uint vid) -> vec3d { return mesh.vert(vid); };
        PlanarDisk pre;
        if(!parameterize_soup(pre_soup, loop, pre_pos, pre)) return false;
        std::unordered_map<uint,uint> pre_local;
        for(uint i=0;i<pre.verts.size();++i) pre_local[pre.verts[i]] = i;

        //-------------- synthetic post-collapse star --------------
        // every surviving face is a star face not incident to the contracted
        // edge. Index them by their link edge (the edge opposite the pivot).
        std::unordered_map<uint64_t,uint> link_edge_face;
        for(uint pid : star)
        {
            if(edge_face_set.count(pid)) continue;
            auto vv = mesh.poly_verts_id(pid);
            for(int k=0;k<3;++k)
            {
                uint a = vv[k], b = vv[(k+1)%3];
                if(a != v0 && a != v1 && b != v0 && b != v1)
                    link_edge_face[edge_key(a,b)] = pid;
            }
        }
        if(link_edge_face.size() != loop.size()) return false;

        // post triangles reuse the REAL face windings, with removed replaced by
        // kept: this is exactly what the production collapse will produce.
        std::vector<Tri3> post_soup;
        std::vector<uint> post_old_face; // parallel to post_soup
        for(size_t i=0;i<loop.size();++i)
        {
            uint a = loop[i], b = loop[(i+1)%loop.size()];
            uint old_pid = link_edge_face.at(edge_key(a,b));
            auto vv = mesh.poly_verts_id(old_pid);
            Tri3 t;
            for(int k=0;k<3;++k) t[k] = (vv[k] == removed) ? kept : vv[k];
            post_soup.push_back(t);
            post_old_face.push_back(old_pid);
        }
        auto post_pos = [&](uint vid) -> vec3d
        {
            return (vid == kept) ? kept_new_pos : mesh.vert(vid);
        };
        PlanarDisk post;
        if(!parameterize_soup(post_soup, loop, post_pos, post)) return false;
        std::unordered_map<uint,uint> post_local;
        for(uint i=0;i<post.verts.size();++i) post_local[post.verts[i]] = i;

        // the shared boundary must be pinned identically in both disks
        for(size_t i=0;i<loop.size();++i)
        {
            const vec2d & pa = pre.uv[pre_local.at(loop[i])];
            const vec2d & pb = post.uv[post_local.at(loop[i])];
            if((pa - pb).norm() > 1e-9) return false;
        }

        //-------------- source-side planar triangles (existing patches) --------------
        std::vector<SourcePlanarTri> src_tris;
        for(uint pid : affected_patches)
        {
            const CorrespondencePatch & pa = patches[pid];
            uint cf = pa.currentFace;
            auto cv = mesh.poly_verts_id(cf);
            std::array<vec2d,3> cf_uv;
            for(int k=0;k<3;++k)
            {
                auto it = pre_local.find(cv[k]);
                if(it == pre_local.end()) return false;
                cf_uv[k] = pre.uv[it->second];
            }
            SourcePlanarTri st;
            st.sf = pa.sourceFace;
            for(int k=0;k<3;++k)
            {
                const Bary3 & w = pa.currentBary[k]; // current-face bary of source corner k
                st.uv[k] = w[0]*cf_uv[0] + w[1]*cf_uv[1] + w[2]*cf_uv[2];
                // this planar corner maps back to the source surface at the
                // patch's own source-face bary location (composition chain)
                st.bary[k] = pa.sourceBary[k];
            }
            if(signed_area2d(st.uv[0], st.uv[1], st.uv[2]) <= AREA_EPS2D) return false;
            src_tris.push_back(st);
        }

        //-------------- current-side planar triangles (post fan) --------------
        std::vector<CurrentPlanarTri> cur_tris;
        for(size_t i=0;i<post_soup.size();++i)
        {
            CurrentPlanarTri ct;
            ct.oldFace = post_old_face[i];
            const Tri3 & g = post_soup[i];
            ct.uv = { post.uv[post_local.at(g[0])],
                      post.uv[post_local.at(g[1])],
                      post.uv[post_local.at(g[2])] };
            cur_tris.push_back(ct);
        }

        //-------------- planar overlay by pairwise triangle clipping --------------
        // The two planar triangulations cover the same disk. For every source
        // patch triangle and post-collapse current triangle, the Sutherland-
        // Hodgman intersection is a (possibly empty) convex polygon. Together
        // these polygons form an exact common refinement without gaps or
        // overlaps; existing patches are reused (never re-sampled) via the
        // barycentric composition chain in SourcePlanarTri.
        struct Mini
        {
            uint sf, st_idx, ct_idx, old_cf;
            std::array<vec2d,3> corners;
        };
        std::vector<Mini> minis;
        for(size_t ci=0; ci<cur_tris.size(); ++ci)
        {
            const CurrentPlanarTri & ct = cur_tris[ci];
            for(size_t si=0; si<src_tris.size(); ++si)
            {
                const SourcePlanarTri & st = src_tris[si];
                std::vector<vec2d> poly = clip_polygon_triangle(
                    {ct.uv[0], ct.uv[1], ct.uv[2]}, st.uv);
                if(poly.size() < 3) continue;
                // clip symmetrically so every corner is guaranteed on both sides
                poly = clip_polygon_triangle(poly, ct.uv);
                if(poly.size() < 3) continue;
                for(size_t k=1;k+1<poly.size();++k)
                {
                    Mini mn;
                    mn.st_idx = uint(si);
                    mn.ct_idx = uint(ci);
                    mn.sf     = st.sf;
                    mn.old_cf = ct.oldFace;
                    mn.corners = {poly[0], poly[k], poly[k+1]};
                    if(signed_area2d(mn.corners[0], mn.corners[1], mn.corners[2]) <= AREA_EPS2D)
                        continue;
                    minis.push_back(mn);
                }
            }
        }
        if(minis.empty()) return false;

        // build candidate patches (still with pre-collapse face ids). The
        // current-side bary values are stored in canonical post-soup vertex
        // order (kept, link edge endpoints) and permuted to the dense post
        // mesh face order after the collapse.
        struct FreshPatch
        {
            CorrespondencePatch pa;
            std::array<Bary3,3>  curBaryCanonical;
        };
        std::vector<FreshPatch> fresh;
        for(const Mini & mn : minis)
        {
            const SourcePlanarTri & st = src_tris[mn.st_idx];
            const CurrentPlanarTri & ct = cur_tris[mn.ct_idx];

            FreshPatch fp;
            fp.pa.sourceFace = mn.sf;
            fp.pa.currentFace = mn.old_cf; // remapped after the collapse
            for(int k=0;k<3;++k)
            {
                Bary3 ws = bary2d(st.uv, mn.corners[k]);
                Bary3 wc = bary2d(ct.uv, mn.corners[k]);
                // clipping guarantees containment; reject only gross failures,
                // let snap_bary() remove floating point sign noise on shared edges
                bool ws_ok = ws[0]>=-CLASS_EPS && ws[1]>=-CLASS_EPS && ws[2]>=-CLASS_EPS;
                bool wc_ok = wc[0]>=-CLASS_EPS && wc[1]>=-CLASS_EPS && wc[2]>=-CLASS_EPS;
                if(!ws_ok || !wc_ok) return false;
                // source side: compose the planar weight with the patch chain
                Bary3 src{0.0,0.0,0.0};
                for(int r=0;r<3;++r) for(int c=0;c<3;++c) src[c] += ws[r]*st.bary[r][c];
                fp.pa.sourceBary[k]     = snap_bary(src);
                fp.curBaryCanonical[k]  = snap_bary(wc);
            }
            // strictly positive 3D area on the source side
            auto st3 = source_tri(fp.pa.sourceFace);
            std::array<vec3d,3> src_corners;
            for(int k=0;k<3;++k) src_corners[k] = eval3d(st3, fp.pa.sourceBary[k]);
            if(tri3_area(src_corners) <= 0.0) return false;
            fresh.push_back(fp);
        }

        // the arrangement is an exact common refinement: the cell area assigned
        // to every parent triangle must equal that triangle's area (FP slack).
        std::map<uint,double> src_cov, src_ref;
        std::map<uint,double> cur_cov, cur_ref;
        for(uint i=0;i<src_tris.size();++i)
            src_ref[i] = std::fabs(signed_area2d(src_tris[i].uv[0], src_tris[i].uv[1], src_tris[i].uv[2]));
        for(uint i=0;i<cur_tris.size();++i)
            cur_ref[i] = std::fabs(signed_area2d(cur_tris[i].uv[0], cur_tris[i].uv[1], cur_tris[i].uv[2]));
        for(const Mini & mn : minis)
        {
            double a = std::fabs(signed_area2d(mn.corners[0], mn.corners[1], mn.corners[2]));
            src_cov[mn.st_idx] += a;
            cur_cov[mn.ct_idx] += a;
        }
        for(const auto & kv : src_ref)
        {
            if(!src_cov.count(kv.first)) return false;
            if(std::fabs(src_cov[kv.first]-kv.second) > COVER_TOL * std::max(1.0, kv.second)) return false;
        }
        for(const auto & kv : cur_ref)
        {
            if(!cur_cov.count(kv.first)) return false;
            if(std::fabs(cur_cov[kv.first]-kv.second) > COVER_TOL * std::max(1.0, kv.second)) return false;
        }

        // canonical post-soup vertex order and predicted dense face order of
        // every surviving face. The production collapse substitutes removed by
        // kept inside the old face's vertex list (order preserved), so the
        // post-collapse corner order is predictable without touching the mesh.
        std::unordered_map<uint,Tri3> canonical_verts;
        std::unordered_map<uint,Tri3> predicted_verts;
        for(size_t i=0;i<post_soup.size();++i)
        {
            uint old_pid = post_old_face[i];
            canonical_verts[old_pid] = post_soup[i];
            auto vv = mesh.poly_verts_id(old_pid);
            Tri3 pred;
            for(int k=0;k<3;++k) pred[k] = (vv[k] == removed) ? kept : vv[k];
            predicted_verts[old_pid] = pred;
        }

        // finalize the current-side bary values (permuted into the predicted
        // dense face order) and run all 3D checks on the synthetic post mesh,
        // still before mutating the real working mesh.
        std::vector<CorrespondencePatch> fresh_final;
        fresh_final.reserve(fresh.size());
        for(FreshPatch & fp : fresh)
        {
            uint old_pid = fp.pa.currentFace;
            Tri3 canon = canonical_verts.at(old_pid);
            Tri3 pred  = predicted_verts.at(old_pid);
            std::array<int,3> perm;
            for(int k=0;k<3;++k)
            {
                perm[k] = -1;
                for(int j=0;j<3;++j) if(pred[k] == canon[j]) { perm[k] = j; break; }
                if(perm[k] < 0) return false;
            }
            for(int k=0;k<3;++k) fp.pa.currentBary[k] = fp.curBaryCanonical[perm[k]];

            // current side 3D corners use the synthetic post positions
            std::array<vec3d,3> predPos{post_pos(pred[0]), post_pos(pred[1]), post_pos(pred[2])};
            std::array<vec3d,3> cur_corners;
            for(int k=0;k<3;++k) cur_corners[k] = eval3d(predPos, fp.pa.currentBary[k]);
            if(tri3_area(cur_corners) <= 0.0) return false;
            fresh_final.push_back(fp.pa);
        }

        // current side 3D coverage on the synthetic surviving star
        {
            std::map<uint,double> got, ref;
            for(const CurrentPlanarTri & ct : cur_tris)
            {
                Tri3 pred = predicted_verts.at(ct.oldFace);
                std::array<vec3d,3> t{post_pos(pred[0]), post_pos(pred[1]), post_pos(pred[2])};
                ref[ct.oldFace] = tri3_area(t);
            }
            for(size_t i=0;i<fresh_final.size();++i)
            {
                const CorrespondencePatch & pa = fresh_final[i];
                uint old_pid = fresh[i].pa.currentFace;
                Tri3 pred = predicted_verts.at(old_pid);
                std::array<vec3d,3> t{post_pos(pred[0]), post_pos(pred[1]), post_pos(pred[2])};
                std::array<vec3d,3> cc;
                for(int k=0;k<3;++k) cc[k] = eval3d(t, pa.currentBary[k]);
                got[old_pid] += tri3_area(cc);
            }
            for(const auto & kv : ref)
            {
                if(!got.count(kv.first)) return false;
                if(std::fabs(got[kv.first]-kv.second) > COVER_TOL * std::max(1.0, kv.second)) return false;
            }
        }
        // source side 3D coverage: every affected source face stays fully covered
        {
            std::map<uint,double> got;
            for(const CorrespondencePatch & pa : fresh_final)
            {
                auto st3 = source_tri(pa.sourceFace);
                std::array<vec3d,3> sc;
                for(int k=0;k<3;++k) sc[k] = eval3d(st3, pa.sourceBary[k]);
                got[pa.sourceFace] += tri3_area(sc);
            }
            for(uint sf : affected_src)
            {
                if(!got.count(sf)) return false;
                double r = tri3_area(source_tri(sf));
                if(std::fabs(got[sf]-r) > COVER_TOL * std::max(1.0, r)) return false;
            }
        }

        // ============ all math and validation succeeded: mutate the mesh ============
        uint nFacesBefore = mesh.num_polys();
        ElementRewriteReport report;
        int kept_back = mesh.edge_collapse(eid, lambda, true, true, report);
        if(kept_back < 0 || uint(kept_back) != kept)
            throw std::runtime_error("SurfaceCorrespondence: pre-validated collapse unexpectedly failed");

        // The collapse adds one fresh face per surviving face incident to
        // removed (in adj_v2p order), then compacts everything with swaps.
        if(report.poly_adds.size() != removed_star_old.size())
            throw std::runtime_error("SurfaceCorrespondence: corrupt rewrite report");
        for(size_t k=0;k<removed_star_old.size();++k)
            reborn_old_to_add[removed_star_old[k]] = report.poly_adds[k];

        uint nSlots = nFacesBefore + uint(report.poly_adds.size());
        std::vector<uint> slot_owner(nSlots);
        for(uint i=0;i<nSlots;++i) slot_owner[i] = i;
        for(auto pr : report.poly_id_swaps) std::swap(slot_owner[pr.first], slot_owner[pr.second]);
        std::vector<uint> slot_map(nSlots, UINT_MAX); // element id -> final dense slot
        for(uint slot=0; slot<mesh.num_polys() && slot<nSlots; ++slot)
            slot_map[slot_owner[slot]] = slot;

        auto map_star_face = [&](uint old_pid) -> uint
        {
            auto it = reborn_old_to_add.find(old_pid);
            uint element = (it == reborn_old_to_add.end()) ? old_pid : it->second;
            return slot_map.at(element);
        };

        // remap the finalized patches to dense post-collapse ids
        for(CorrespondencePatch & pa : fresh_final)
        {
            // currentFace still holds the old id at this point
            uint nf = map_star_face(pa.currentFace);
            if(nf >= mesh.num_polys())
                throw std::runtime_error("SurfaceCorrespondence: corrupt rewrite report");
            pa.currentFace = nf;
        }

        // commit patch replacement
        std::unordered_set<uint> affected_set(affected_patches.begin(), affected_patches.end());
        std::vector<CorrespondencePatch> kept_patches;
        kept_patches.reserve(patches.size());
        for(uint pid=0; pid<patches.size(); ++pid)
        {
            if(affected_set.count(pid)) continue;
            CorrespondencePatch pa = patches[pid];
            uint nf = slot_map.at(pa.currentFace); // unaffected faces were never re-added
            if(nf >= mesh.num_polys())
                throw std::runtime_error("SurfaceCorrespondence: corrupt rewrite report");
            pa.currentFace = nf;
            kept_patches.push_back(pa);
        }
        patches = std::move(kept_patches);
        patches.insert(patches.end(), fresh_final.begin(), fresh_final.end());

        srcIndex.assign(srcFaces.size(), {});
        curIndex.assign(mesh.num_polys(), {});
        for(uint pid=0;pid<patches.size();++pid)
        {
            srcIndex[patches[pid].sourceFace].push_back(pid);
            curIndex[patches[pid].currentFace].push_back(pid);
        }

        // public rewrite report
        lastReport = CollapseRewriteReport();
        lastReport.edge = eid;
        lastReport.keptVert = kept;
        lastReport.removedVert = removed;
        lastReport.removedFaces = edge_faces;
        std::unordered_set<uint> edge_set(edge_faces.begin(), edge_faces.end());
        for(uint old_pid : star)
            if(!edge_set.count(old_pid))
                lastReport.rebornFaces.emplace_back(old_pid, map_star_face(old_pid));
        lastReport.faceIdSwaps = report.poly_id_swaps;
        lastReport.vertIdSwaps = report.vert_id_swaps;
        return true;
    }

    //------------------------------------------------------------------
    // queries
    //------------------------------------------------------------------

    SurfaceQuery toCurrent(uint sf, const Bary3 & bary) const
    {
        if(sf >= srcFaces.size())
            throw std::invalid_argument("SurfaceCorrespondence::toCurrent: bad source face");
        if(!valid_bary(bary))
            throw std::invalid_argument("SurfaceCorrespondence::toCurrent: illegal barycentric weights");
        Bary3 b = snap_bary(bary);
        vec3d p = eval3d(source_tri(sf), b);

        // candidate patches: those attached to the source face and, for edge
        // and vertex points, every face sharing a source edge or vertex with p
        std::unordered_set<uint> cand(srcIndex.at(sf).begin(), srcIndex.at(sf).end());
        const Tri3 & t = srcFaces[sf];
        static const int ee[3][2] = {{0,1},{1,2},{2,0}};
        for(int k=0;k<3;++k)
        {
            if(b[(k+2)%3] > PARAM_EPS) continue; // vertex opposite edge k is present -> not on that edge
            auto it = srcEdgeFaces.find(edge_key(t[ee[k][0]], t[ee[k][1]]));
            if(it != srcEdgeFaces.end())
                for(uint nf : it->second)
                    for(uint pid : srcIndex.at(nf)) cand.insert(pid);
        }
        for(int k=0;k<3;++k)
        {
            // vertex point: include the whole immutable source vertex star
            int nonzero = 0; for(double w:b) if(w > PARAM_EPS) ++nonzero;
            if(nonzero == 1 && b[k] > 1.0 - PARAM_EPS)
                for(uint nf : srcVertFaces[t[k]])
                    for(uint pid : srcIndex.at(nf)) cand.insert(pid);
        }

        bool found = false;
        SurfaceQuery best;
        for(uint pid : cand)
        {
            const CorrespondencePatch & pa = patches[pid];
            auto st = source_tri(pa.sourceFace);
            std::array<vec3d,3> corners;
            for(int k=0;k<3;++k) corners[k] = eval3d(st, pa.sourceBary[k]);
            Bary3 local = bary3d(corners, p);
            if(!inside2d(local)) continue;
            Bary3 out{0,0,0};
            for(int k=0;k<3;++k)
            {
                out[0] += local[k] * pa.currentBary[k][0];
                out[1] += local[k] * pa.currentBary[k][1];
                out[2] += local[k] * pa.currentBary[k][2];
            }
            SurfaceQuery q;
            q.face = pa.currentFace;
            q.bary = snap_bary(out);
            if(!found || q.face < best.face) { best = q; found = true; }
        }
        if(!found) throw std::runtime_error("SurfaceCorrespondence::toCurrent: point outside the partition");
        return best;
    }

    SurfaceQuery toSource(uint cf, const Bary3 & bary) const
    {
        if(cf >= mesh.num_polys())
            throw std::invalid_argument("SurfaceCorrespondence::toSource: bad current face");
        if(!valid_bary(bary))
            throw std::invalid_argument("SurfaceCorrespondence::toSource: illegal barycentric weights");
        Bary3 b = snap_bary(bary);
        vec3d p = eval3d(current_tri(cf), b);

        // expand candidates across current mesh edges/vertices for edge/vertex points
        std::unordered_set<uint> cand(curIndex.at(cf).begin(), curIndex.at(cf).end());
        auto vv = mesh.poly_verts_id(cf);
        Bary3 edge_bary = bary3d(current_tri(cf), p);
        for(uint eid : mesh.adj_p2e(cf))
        {
            uint a = mesh.edge_vert_id(eid,0), c = mesh.edge_vert_id(eid,1);
            int opp = -1;
            for(int k=0;k<3;++k) if(vv[k] != a && vv[k] != c) opp = k;
            if(opp < 0) continue;
            if(edge_bary[opp] > PARAM_EPS) continue; // p not on this edge
            for(uint nf : mesh.adj_e2p(eid))
                for(uint pid : curIndex.at(nf)) cand.insert(pid);
        }
        for(int k=0;k<3;++k)
        {
            // vertex point: include every current face incident to that vertex
            int nonzero = 0; for(double w:b) if(w > PARAM_EPS) ++nonzero;
            if(nonzero == 1 && b[k] > 1.0 - PARAM_EPS)
                for(uint nf : mesh.adj_v2p(vv[k]))
                    for(uint pid : curIndex.at(nf)) cand.insert(pid);
        }

        bool found = false;
        SurfaceQuery best;
        for(uint pid : cand)
        {
            const CorrespondencePatch & pa = patches[pid];
            auto ct = current_tri(pa.currentFace);
            std::array<vec3d,3> corners;
            for(int k=0;k<3;++k) corners[k] = eval3d(ct, pa.currentBary[k]);
            Bary3 local = bary3d(corners, p);
            if(!inside2d(local)) continue;
            Bary3 out{0,0,0};
            for(int k=0;k<3;++k)
            {
                out[0] += local[k] * pa.sourceBary[k][0];
                out[1] += local[k] * pa.sourceBary[k][1];
                out[2] += local[k] * pa.sourceBary[k][2];
            }
            SurfaceQuery q;
            q.face = pa.sourceFace;
            q.bary = snap_bary(out);
            if(!found || q.face < best.face) { best = q; found = true; }
        }
        if(!found) throw std::runtime_error("SurfaceCorrespondence::toSource: point outside the partition");
        return best;
    }

    vec3d source_point(uint sf, const Bary3 & bary) const
    {
        if(sf >= srcFaces.size())
            throw std::invalid_argument("SurfaceCorrespondence::source_point: bad source face");
        if(!valid_bary(bary))
            throw std::invalid_argument("SurfaceCorrespondence::source_point: illegal barycentric weights");
        return eval3d(source_tri(sf), snap_bary(bary));
    }
};

//::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::

SurfaceCorrespondence::SurfaceCorrespondence(const Trimesh<> & source) : impl_(new Impl(source)) {}
SurfaceCorrespondence::~SurfaceCorrespondence() { delete impl_; }

const Trimesh<> & SurfaceCorrespondence::mesh() const { return impl_->mesh; }
uint SurfaceCorrespondence::num_source_faces() const { return uint(impl_->srcFaces.size()); }
uint SurfaceCorrespondence::num_current_faces() const { return impl_->mesh.num_polys(); }
bool SurfaceCorrespondence::collapse(uint eid, double lambda) { return impl_->collapse(eid, lambda); }
const std::vector<CorrespondencePatch> & SurfaceCorrespondence::patches() const { return impl_->patches; }

SurfaceQuery SurfaceCorrespondence::toCurrent(uint sourceFace, const std::array<double,3> & bary) const
{ return impl_->toCurrent(sourceFace, bary); }

SurfaceQuery SurfaceCorrespondence::toSource(uint currentFace, const std::array<double,3> & bary) const
{ return impl_->toSource(currentFace, bary); }

vec3d SurfaceCorrespondence::source_point(uint sourceFace, const std::array<double,3> & bary) const
{ return impl_->source_point(sourceFace, bary); }

const CollapseRewriteReport & SurfaceCorrespondence::last_report() const { return impl_->lastReport; }

}
