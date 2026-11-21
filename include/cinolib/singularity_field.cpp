/********************************************************************************
*  This file is part of CinoLib                                                 *
*  Copyright(C) 2016: Marco Livesu                                              *
*                                                                               *
*  The MIT License                                                              *
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
#include <cinolib/singularity_field.h>
#include <cinolib/homotopy_basis.h>
#include <cmath>
#include <queue>
#include <limits>
#include <stdexcept>

namespace cinolib
{

namespace
{
    inline double sf_pi() { return std::acos(-1.0); }

    // rotation of v around the unit axis by the angle with the given sine/cosine (Rodrigues)
    inline vec3d rotate_axis(const vec3d & v, const vec3d & axis, const double cos_a, const double sin_a)
    {
        return v*cos_a + axis.cross(v)*sin_a + axis*(axis.dot(v))*(1.0-cos_a);
    }
}

//::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::

SingularityField::SingularityField(const Trimesh<> & m)
{
    validate_mesh(m);

    mesh_  = m; // independent snapshot: the source mesh may change or die after this point
    nv_    = mesh_.num_verts();
    ne_    = mesh_.num_edges();
    nf_    = mesh_.num_polys();
    chi_   = int(nv_) - int(ne_) + int(nf_);
    genus_ = uint((2-chi_)/2);

    build_frames();
    build_cycles();
    build_connection();
    build_system();

    diag_.matrix_rows     = uint(S_.rows());
    diag_.matrix_nonzeros = uint(S_.nonZeros());
}

//::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::

void SingularityField::validate_mesh(const Trimesh<> & m) const
{
    if(m.num_verts()==0 || m.num_polys()==0)
        throw std::invalid_argument("SingularityField: empty mesh");

    // finite coordinates
    for(uint vid=0; vid<m.num_verts(); ++vid)
    {
        const vec3d & p = m.vert(vid);
        if(!std::isfinite(p.x()) || !std::isfinite(p.y()) || !std::isfinite(p.z()))
            throw std::invalid_argument("SingularityField: non finite vertex coordinates");
    }

    // non degenerate faces
    for(uint pid=0; pid<m.num_polys(); ++pid)
    {
        const vec3d & p0 = m.poly_vert(pid,0);
        const vec3d & p1 = m.poly_vert(pid,1);
        const vec3d & p2 = m.poly_vert(pid,2);
        vec3d n = (p1-p0).cross(p2-p0);
        if(!std::isfinite(n.norm_sqrd()) || n.norm_sqrd()==0.0)
            throw std::invalid_argument("SingularityField: degenerate face");
    }

    // no isolated vertices
    for(uint vid=0; vid<m.num_verts(); ++vid)
    {
        if(m.adj_v2e(vid).empty())
            throw std::invalid_argument("SingularityField: isolated vertex");
    }

    // closed and edge-manifold
    for(uint eid=0; eid<m.num_edges(); ++eid)
    {
        if(m.adj_e2p(eid).size()!=2)
            throw std::invalid_argument("SingularityField: mesh is not closed or has non manifold edges");
    }

    // vertex-manifold
    for(uint vid=0; vid<m.num_verts(); ++vid)
    {
        if(!m.vert_is_manifold(vid))
            throw std::invalid_argument("SingularityField: non manifold vertex");
    }

    // consistently oriented
    for(uint eid=0; eid<m.num_edges(); ++eid)
    {
        uint p0 = m.adj_e2p(eid).at(0);
        uint p1 = m.adj_e2p(eid).at(1);
        if(m.edge_is_CCW(eid,p0)==m.edge_is_CCW(eid,p1))
            throw std::invalid_argument("SingularityField: mesh is not coherently oriented");
    }

    // connected (face BFS)
    {
        std::vector<bool> vis(m.num_polys(),false);
        std::queue<uint>  q;
        q.push(0); vis[0]=true;
        uint count=0;
        while(!q.empty())
        {
            uint pid=q.front(); q.pop(); ++count;
            for(uint nbr : m.adj_p2p(pid))
            {
                if(!vis[nbr]) { vis[nbr]=true; q.push(nbr); }
            }
        }
        if(count!=m.num_polys())
            throw std::invalid_argument("SingularityField: mesh is not connected");
    }

    // genus 0 or 1
    int chi = int(m.num_verts()) - int(m.num_edges()) + int(m.num_polys());
    if(chi!=2 && chi!=0)
        throw std::invalid_argument("SingularityField: only genus 0 and 1 meshes are supported");
}

//::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::

void SingularityField::build_frames()
{
    frames_.resize(nf_);
    normals_.resize(nf_);
    edge_faces_.resize(ne_);
    face_nbrs_.resize(nf_);

    for(uint pid=0; pid<nf_; ++pid)
    {
        const vec3d & p0 = mesh_.poly_vert(pid,0);
        const vec3d & p1 = mesh_.poly_vert(pid,1);
        const vec3d & p2 = mesh_.poly_vert(pid,2);
        vec3d u = p1-p0;
        vec3d n = (p1-p0).cross(p2-p0);
        u.normalize();
        n.normalize();
        frames_[pid].u = u;
        frames_[pid].v = n.cross(u); // right handed: u x v = n
        normals_[pid]  = n;
    }

    for(uint eid=0; eid<ne_; ++eid)
    {
        uint f0 = mesh_.adj_e2p(eid).at(0);
        uint f1 = mesh_.adj_e2p(eid).at(1);
        if(f1<f0) std::swap(f0,f1);
        edge_faces_[eid] = std::make_pair(f0,f1);
        face_nbrs_[f0].push_back(std::make_pair(eid,f1));
        face_nbrs_[f1].push_back(std::make_pair(eid,f0));
    }
}

//::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::

void SingularityField::build_cycles()
{
    // tree/cotree decomposition on the private copy
    HomotopyBasisData data;
    data.root             = 0;
    data.globally_shortest = false;
    data.detach_loops     = false;
    homotopy_basis(mesh_,data);

    // generator edges, in increasing id order
    std::vector<uint> generators;
    for(uint eid=0; eid<ne_; ++eid)
    {
        if(!data.tree.at(eid) && !data.cotree.at(eid)) generators.push_back(eid);
    }
    if(generators.size()!=2*genus_)
        throw std::invalid_argument("SingularityField: unexpected number of generators");

    // dual adjacency restricted to cotree edges (a spanning tree of the dual graph)
    std::vector<std::vector<std::pair<uint,uint>>> dual(nf_);
    for(uint eid=0; eid<ne_; ++eid)
    {
        if(!data.cotree.at(eid)) continue;
        uint f0 = edge_faces_[eid].first;
        uint f1 = edge_faces_[eid].second;
        dual[f0].push_back(std::make_pair(f1,eid));
        dual[f1].push_back(std::make_pair(f0,eid));
    }

    const uint NONE = std::numeric_limits<uint>::max();
    for(uint gen : generators)
    {
        uint f_small = edge_faces_[gen].first;
        uint f_large = edge_faces_[gen].second;

        // unique cotree path from f_large back to f_small
        std::vector<uint> parent(nf_,NONE), parent_edge(nf_,NONE);
        std::queue<uint>  Q;
        parent[f_large] = f_large;
        Q.push(f_large);
        while(!Q.empty())
        {
            uint cur=Q.front(); Q.pop();
            if(cur==f_small) break;
            for(auto & nbr : dual[cur])
            {
                if(parent[nbr.first]==NONE)
                {
                    parent[nbr.first]      = cur;
                    parent_edge[nbr.first] = nbr.second;
                    Q.push(nbr.first);
                }
            }
        }

        Cycle cyc;
        cyc.generator_edge = gen;
        cyc.edges.push_back(gen); // directed from lower to higher face id
        cyc.signs.push_back(+1);
        std::vector<uint> rev_edges, rev_from, rev_to;
        uint cur = f_small;
        while(cur!=f_large)
        {
            uint par = parent.at(cur);
            rev_edges.push_back(parent_edge.at(cur));
            rev_from.push_back(par);
            rev_to.push_back(cur);
            cur = par;
        }
        for(int i=int(rev_edges.size())-1; i>=0; --i)
        {
            cyc.edges.push_back(rev_edges[i]);
            cyc.signs.push_back(rev_from[i]<rev_to[i] ? +1 : -1);
        }
        cycles_.push_back(cyc);
    }
}

//::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::

void SingularityField::build_connection()
{
    omega_.resize(ne_);
    for(uint eid=0; eid<ne_; ++eid)
    {
        uint f = edge_faces_[eid].first;
        uint g = edge_faces_[eid].second;

        // unfold the frame of g onto the plane of f: rotate around the shared
        // edge axis by the signed angle taking n_g to n_f
        vec3d axis = mesh_.edge_vert(eid,1) - mesh_.edge_vert(eid,0);
        axis.normalize();
        double cos_a = normals_[g].dot(normals_[f]);
        if(cos_a> 1.0) cos_a= 1.0;
        if(cos_a<-1.0) cos_a=-1.0;
        double sin_a = normals_[g].cross(normals_[f]).dot(axis);
        vec3d ug_unf = rotate_axis(frames_[g].u, axis, cos_a, sin_a);

        // directed Levi-Civita transport angle (principal value in (-pi,pi]),
        // antisymmetric under reversal; with this convention a Levi-Civita
        // parallel field satisfies theta_g = theta_f + omega_fg and the sum
        // of omega around a positive vertex ring equals the angle defect mod 2pi
        omega_[eid] = -std::atan2(frames_[f].u.cross(ug_unf).dot(normals_[f]),
                                  frames_[f].u.dot(ug_unf));
    }
}

//::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::

int SingularityField::vertex_edge_sign(const uint vid, const uint eid) const
{
    uint f0 = edge_faces_[eid].first;
    uint f1 = edge_faces_[eid].second;
    uint w  = (mesh_.edge_vert_id(eid,0)==vid) ? mesh_.edge_vert_id(eid,1)
                                               : mesh_.edge_vert_id(eid,0);
    // the positive ring of vid crosses the dual edge from the face winding
    // w->vid to the face winding vid->w; the canonical direction is f0->f1
    bool f0_winds_vw = face_winds_edge(f0,vid,w);
    uint f_wv = f0_winds_vw ? f1 : f0;
    return (f_wv==f0) ? +1 : -1;
}

//::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::

bool SingularityField::face_winds_edge(const uint fid, const uint v_from, const uint v_to) const
{
    for(uint i=0; i<3; ++i)
    {
        if(mesh_.poly_vert_id(fid,i)==v_from && mesh_.poly_vert_id(fid,(i+1)%3)==v_to) return true;
    }
    return false;
}

//::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::

void SingularityField::build_system()
{
    const double pi = sf_pi();

    // angle defects (real-valued lift, never reduced mod 2*pi)
    angle_defect_.assign(nv_,0.0);
    for(uint pid=0; pid<nf_; ++pid)
    {
        for(uint i=0; i<3; ++i)
        {
            uint   vid  = mesh_.poly_vert_id(pid,i);
            vec3d  a    = mesh_.poly_vert(pid,(i+1)%3) - mesh_.poly_vert(pid,i);
            vec3d  b    = mesh_.poly_vert(pid,(i+2)%3) - mesh_.poly_vert(pid,i);
            double ang  = std::atan2(a.cross(b).norm(), a.dot(b));
            angle_defect_[vid] -= ang;
        }
    }
    for(uint vid=0; vid<nv_; ++vid) angle_defect_[vid] += 2.0*pi;

    // signed sums of omega along the basis cycles
    cycle_omega_.assign(cycles_.size(),0.0);
    for(uint j=0; j<cycles_.size(); ++j)
    {
        double s=0.0;
        for(uint i=0; i<cycles_[j].edges.size(); ++i)
            s += double(cycles_[j].signs[i])*omega_[cycles_[j].edges[i]];
        cycle_omega_[j] = s;
    }

    // constraint matrix: rows 0..nv_-2 are the positive vertex rings (the
    // equation of vertex dropped_vertex_ is redundant and dropped), the
    // remaining rows are the basis cycles
    dropped_vertex_ = nv_-1;
    uint rows = (nv_-1) + uint(cycles_.size());

    std::vector<Eigen::Triplet<double>> trips;
    std::vector<std::vector<std::pair<uint,int>>> cols(ne_);
    for(uint vid=0; vid<nv_; ++vid)
    {
        if(vid==dropped_vertex_) continue;
        for(uint eid : mesh_.adj_v2e(vid))
        {
            int s = vertex_edge_sign(vid,eid);
            trips.push_back(Eigen::Triplet<double>(vid,eid,double(s)));
            cols[eid].push_back(std::make_pair(vid,s));
        }
    }
    for(uint j=0; j<cycles_.size(); ++j)
    {
        uint row = (nv_-1)+j;
        for(uint i=0; i<cycles_[j].edges.size(); ++i)
        {
            int s = cycles_[j].signs[i];
            trips.push_back(Eigen::Triplet<double>(row,cycles_[j].edges[i],double(s)));
            cols[cycles_[j].edges[i]].push_back(std::make_pair(row,s));
        }
    }
    A_.resize(rows,ne_);
    A_.setFromTriplets(trips.begin(),trips.end());

    // normal equations S = A*A^T, assembled sparsely column by column
    std::vector<Eigen::Triplet<double>> strips;
    for(uint eid=0; eid<ne_; ++eid)
    {
        for(auto & ra : cols[eid])
        for(auto & rb : cols[eid])
        {
            strips.push_back(Eigen::Triplet<double>(ra.first,rb.first,double(ra.second*rb.second)));
        }
    }
    S_.resize(rows,rows);
    S_.setFromTriplets(strips.begin(),strips.end());

    solver_.compute(S_);
    if(solver_.info()==Eigen::Success)
    {
        factored_ = true;
        ++factorizations_;
    }
}

//::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::

bool SingularityField::solve(const std::vector<int> & q,
                             const std::vector<int> & periods,
                             const vec3d            & anchor)
{
    const double pi = sf_pi();

    // ---- input validation (invalid input throws) ----
    if(q.size()!=nv_)
        throw std::invalid_argument("SingularityField::solve: q must have one entry per vertex");
    if(periods.size()!=cycles_.size())
        throw std::invalid_argument("SingularityField::solve: periods must have 2*genus entries");
    long qsum=0;
    for(int x : q) qsum+=x;
    if(qsum!=4L*chi_)
        throw std::invalid_argument("SingularityField::solve: sum(q) must equal 4*Euler_characteristic");
    if(!std::isfinite(anchor.x()) || !std::isfinite(anchor.y()) || !std::isfinite(anchor.z()))
        throw std::invalid_argument("SingularityField::solve: anchor must be finite");
    double anorm = anchor.norm();
    if(!(anorm>0.0))
        throw std::invalid_argument("SingularityField::solve: anchor must be nonzero");
    if(std::abs(anchor.dot(normals_[0]))>1e-6*anorm)
        throw std::invalid_argument("SingularityField::solve: anchor must be tangent to face 0");

    if(!factored_) return false;

    uint rows = uint(A_.rows());

    // ---- right hand side ----
    Eigen::VectorXd b(rows);
    for(uint vid=0; vid<nv_-1; ++vid) b[vid] = double(q[vid])*pi/2.0 - angle_defect_[vid];
    for(uint j=0; j<cycles_.size(); ++j)
        b[(nv_-1)+j] = double(periods[j])*pi/2.0 - cycle_omega_[j];

    // ---- minimum norm solution beta = A^T (A A^T)^{-1} b ----
    Eigen::VectorXd lambda = solver_.solve(b);
    Eigen::VectorXd beta   = A_.transpose()*lambda;
    if(beta.size()!=Eigen::Index(ne_)) return false;

    // ---- residuals: all constraints, dropped vertex equation checked independently ----
    Eigen::VectorXd res = A_*beta - b;
    double max_res = (rows>0) ? res.cwiseAbs().maxCoeff() : 0.0;
    double dropped = 0.0;
    for(uint eid : mesh_.adj_v2e(dropped_vertex_)) dropped += double(vertex_edge_sign(dropped_vertex_,eid))*beta[eid];
    dropped -= double(q[dropped_vertex_])*pi/2.0 - angle_defect_[dropped_vertex_];
    if(std::abs(dropped)>max_res) max_res = std::abs(dropped);
    if(!std::isfinite(max_res) || max_res>1e-8) return false;

    // ---- integrate the corrected connection from the anchor ----
    std::vector<double> theta(nf_,0.0);
    {
        std::vector<bool> vis(nf_,false);
        std::queue<uint>  Q;
        theta[0] = std::atan2(anchor.dot(frames_[0].v), anchor.dot(frames_[0].u));
        vis[0]=true;
        Q.push(0);
        while(!Q.empty())
        {
            uint f=Q.front(); Q.pop();
            for(auto & nbr : face_nbrs_[f])
            {
                uint eid = nbr.first;
                uint g   = nbr.second;
                if(vis[g]) continue;
                double w = omega_[eid]+beta[eid];
                theta[g] = theta[f] + (f<g ? w : -w);
                vis[g]=true;
                Q.push(g);
            }
        }
    }

    // ---- integer matching per directed dual edge ----
    std::vector<int> qt(ne_,0);
    double max_match = 0.0;
    for(uint eid=0; eid<ne_; ++eid)
    {
        uint   f0 = edge_faces_[eid].first;
        uint   f1 = edge_faces_[eid].second;
        double d  = theta[f1]-theta[f0]-omega_[eid]-beta[eid];
        int    k  = int(std::llround(d/(pi/2.0)));
        double er = std::abs(d-double(k)*pi/2.0);
        if(er>max_match) max_match=er;
        qt[eid]=k;
    }
    if(!std::isfinite(max_match) || max_match>1e-8) return false;

    // ---- per face unit tangent representatives ----
    VectorField field(nf_);
    for(uint pid=0; pid<nf_; ++pid)
    {
        vec3d dir = frames_[pid].u*std::cos(theta[pid]) + frames_[pid].v*std::sin(theta[pid]);
        dir.normalize();
        field.set(pid,dir);
    }

    // ---- commit ----
    beta_.assign(beta.data(),beta.data()+ne_);
    qt_    = qt;
    field_ = field;
    diag_.max_constraint_residual = max_res;
    diag_.dropped_vertex_residual = std::abs(dropped);
    diag_.max_matching_error      = max_match;
    diag_.energy                  = 0.5*beta.squaredNorm();
    diag_.sparse_factorizations   = factorizations_;
    solved_ = true;
    return true;
}

//::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::

void SingularityField::require_solved() const
{
    if(!solved_) throw std::logic_error("SingularityField: no successful solve yet");
}

//::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::

uint SingularityField::num_verts()            const { return nv_;    }
uint SingularityField::num_edges()            const { return ne_;    }
uint SingularityField::num_faces()            const { return nf_;    }
uint SingularityField::genus()                const { return genus_; }
int  SingularityField::euler_characteristic() const { return chi_;   }

//::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::

const std::vector<SingularityField::Frame> & SingularityField::frames() const { return frames_; }

//::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::

const vec3d & SingularityField::face_normal(const uint fid) const { return normals_.at(fid); }

//::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::

const std::vector<double> & SingularityField::connections() const { return omega_; }

//::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::

const std::vector<SingularityField::Cycle> & SingularityField::cycles() const { return cycles_; }

//::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::

std::pair<uint,uint> SingularityField::edge_faces(const uint eid) const { return edge_faces_.at(eid); }

//::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::

const std::vector<double> & SingularityField::corrections()  const { require_solved(); return beta_;  }
const std::vector<int>    & SingularityField::quarter_turns() const { require_solved(); return qt_;   }
const VectorField         & SingularityField::field()        const { require_solved(); return field_; }
const SingularityField::Diagnostics & SingularityField::diagnostics() const { require_solved(); return diag_; }

//::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::

double SingularityField::connection(const uint eid, const uint from, const uint to) const
{
    auto ef = edge_faces_.at(eid);
    if(from==ef.first && to==ef.second) return  omega_.at(eid);
    if(from==ef.second && to==ef.first) return -omega_.at(eid);
    throw std::invalid_argument("SingularityField::connection: faces are not the two faces of the edge");
}

//::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::

double SingularityField::correction(const uint eid, const uint from, const uint to) const
{
    require_solved();
    auto ef = edge_faces_.at(eid);
    if(from==ef.first && to==ef.second) return  beta_.at(eid);
    if(from==ef.second && to==ef.first) return -beta_.at(eid);
    throw std::invalid_argument("SingularityField::correction: faces are not the two faces of the edge");
}

//::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::

int SingularityField::quarter_turn(const uint eid, const uint from, const uint to) const
{
    require_solved();
    auto ef = edge_faces_.at(eid);
    if(from==ef.first && to==ef.second) return  qt_.at(eid);
    if(from==ef.second && to==ef.first) return -qt_.at(eid);
    throw std::invalid_argument("SingularityField::quarter_turn: faces are not the two faces of the edge");
}

}
