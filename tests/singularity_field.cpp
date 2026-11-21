// Tests for cinolib::SingularityField (see include/cinolib/singularity_field.h).
//
// Registered groups (driven by task_support/driver.c):
//   connection     : tangent frames, Levi-Civita transport, basis cycles
//   singularities  : prescribed indices, exact constraints, optimality, anchor, recovery
//   periods        : integer handle periods on a genus 1 mesh
//   reuse          : factorization reuse, sparse storage, snapshot independence
//
// All checks are independent recomputations from the published frames,
// connections, corrections, quarter turns and field: faces, vertex rings and
// non contractible cycles are unfolded here independently of the library.

#include "task_support/check.h"

#include <cinolib/singularity_field.h>
#include <cinolib/meshes/trimesh.h>

#include <Eigen/Dense>

#include <cmath>
#include <climits>
#include <limits>
#include <map>
#include <vector>
#include <stdexcept>

using namespace cinolib;

namespace
{

const double PI = std::acos(-1.0);

//::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
// mesh builders (all asymmetric)
//::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::

Trimesh<> make_sphere(const uint subdiv)
{
    std::vector<vec3d> p = {vec3d(1,0,0),vec3d(-1,0,0),vec3d(0,1,0),vec3d(0,-1,0),vec3d(0,0,1),vec3d(0,0,-1)};
    std::vector<std::vector<uint>> f = {{0,2,4},{2,1,4},{1,3,4},{3,0,4},
                                        {2,0,5},{1,2,5},{3,1,5},{0,3,5}};
    for(uint s=0; s<subdiv; ++s)
    {
        std::map<std::pair<uint,uint>,uint> mid;
        std::vector<std::vector<uint>> f2;
        auto midpoint=[&](uint a, uint b)->uint
        {
            auto key=std::minmax(a,b);
            auto it=mid.find(key);
            if(it!=mid.end()) return it->second;
            vec3d m=(p[a]+p[b])*0.5;
            uint id=uint(p.size());
            p.push_back(m);
            mid[key]=id;
            return id;
        };
        for(auto & t : f)
        {
            uint a=t[0],b=t[1],c=t[2];
            uint ab=midpoint(a,b),bc=midpoint(b,c),ca=midpoint(c,a);
            f2.push_back({a,ab,ca}); f2.push_back({ab,b,bc});
            f2.push_back({ca,bc,c}); f2.push_back({ab,bc,ca});
        }
        f=f2;
    }
    // project on the unit sphere, then perturb radially (asymmetric)
    for(auto & v : p)
    {
        v.normalize();
        double r = 1.0 + 0.13*std::sin(3*v.x()+1.0)*std::cos(2*v.y()+0.5)
                        + 0.07*std::sin(5*v.z()+2.0);
        v*=r;
    }
    std::vector<uint> ff;
    for(auto & t : f) ff.insert(ff.end(),{t[0],t[1],t[2]});
    return Trimesh<>(p,ff);
}

//::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::

Trimesh<> make_torus(const uint N, const uint M)
{
    std::vector<vec3d> p;
    std::vector<uint>  f;
    for(uint i=0;i<N;++i) for(uint j=0;j<M;++j)
    {
        double u = 2*PI*i/N;
        double v = 2*PI*j/M;
        double R = 3.0 + 0.25*std::sin(2*u+1.0) + 0.10*std::cos(3*v+0.5);
        double r = 1.0 + 0.15*std::cos(3*u+0.3) + 0.05*std::sin(2*v+2.0);
        p.emplace_back((R+r*std::cos(v))*std::cos(u),
                       (R+r*std::cos(v))*std::sin(u),
                        r*std::sin(v));
    }
    for(uint i=0;i<N;++i) for(uint j=0;j<M;++j)
    {
        uint a= i      *M+j;
        uint b=((i+1)%N)*M+j;
        uint c=((i+1)%N)*M+(j+1)%M;
        uint d= i      *M+(j+1)%M;
        f.insert(f.end(),{a,b,c,a,c,d});
    }
    return Trimesh<>(p,f);
}

//::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::

// connected sum of two tori along a removed triangle: genus 2
Trimesh<> make_double_torus()
{
    auto torus=[](std::vector<vec3d> & p, std::vector<std::vector<uint>> & f)
    {
        const uint N=6,M=5;
        for(uint i=0;i<N;++i) for(uint j=0;j<M;++j)
        {
            double u=2*PI*i/N, v=2*PI*j/M;
            p.emplace_back((3+std::cos(v))*std::cos(u),(3+std::cos(v))*std::sin(u),std::sin(v));
        }
        for(uint i=0;i<N;++i) for(uint j=0;j<M;++j)
        {
            uint a=i*M+j,b=((i+1)%N)*M+j,c=((i+1)%N)*M+(j+1)%M,d=i*M+(j+1)%M;
            f.push_back({a,b,c}); f.push_back({a,c,d});
        }
    };
    std::vector<vec3d> pA,pB;
    std::vector<std::vector<uint>> fA,fB;
    torus(pA,fA); torus(pB,fB);

    uint a0=fA[0][0],a1=fA[0][1],a2=fA[0][2];
    uint b0=fB[0][0],b1=fB[0][1],b2=fB[0][2];

    // orientation reversing identification of the two boundary triangles
    std::vector<uint> newid(pB.size());
    uint next=uint(pA.size());
    for(uint i=0;i<pB.size();++i)
    {
        if(i==b0)      newid[i]=a0;
        else if(i==b1) newid[i]=a2;
        else if(i==b2) newid[i]=a1;
        else           newid[i]=next++;
    }
    std::vector<vec3d> p=pA;
    p.resize(next);
    for(uint i=0;i<pB.size();++i) if(newid[i]>=pA.size()) p[newid[i]]=pB[i];

    std::vector<uint> f;
    for(uint t=1;t<fA.size();++t) f.insert(f.end(),{fA[t][0],fA[t][1],fA[t][2]});
    for(uint t=1;t<fB.size();++t) f.insert(f.end(),{newid[fB[t][0]],newid[fB[t][1]],newid[fB[t][2]]});
    return Trimesh<>(p,f);
}

//::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::

Trimesh<> make_tetra()
{
    std::vector<vec3d> p = {vec3d(0,0,0),vec3d(1,0,0),vec3d(0,1,0),vec3d(0,0,1)};
    std::vector<uint>  f = {0,2,1, 0,1,3, 1,2,3, 2,0,3};
    return Trimesh<>(p,f);
}

//::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
// independent geometry helpers
//::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::

// positive (counterclockwise, seen from outside) ring of faces/edges around v
void positive_ring(const Trimesh<> & m, const uint v,
                   std::vector<uint> & faces, std::vector<uint> & edges)
{
    faces.clear(); edges.clear();
    uint f0=m.adj_v2p(v).front(), f=f0;
    do
    {
        faces.push_back(f);
        uint w=UINT_MAX;
        for(uint i=0;i<3;++i)
        {
            uint a=m.poly_vert_id(f,i), b=m.poly_vert_id(f,(i+1)%3);
            if(b==v){ w=a; break; }
        }
        uint e=UINT_MAX;
        for(uint eid : m.adj_v2e(v))
        {
            uint x=m.edge_vert_id(eid,0), y=m.edge_vert_id(eid,1);
            if(x==w||y==w){ e=eid; break; }
        }
        edges.push_back(e);
        uint g0=m.adj_e2p(e).at(0), g1=m.adj_e2p(e).at(1);
        f=(g0==f)?g1:g0;
    }
    while(f!=f0);
}

//::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::

double angle_defect(const Trimesh<> & m, const uint v)
{
    double K=2*PI;
    for(uint f : m.adj_v2p(v))
    {
        uint off=0;
        for(;off<3;++off) if(m.poly_vert_id(f,off)==v) break;
        vec3d a=m.poly_vert(f,(off+1)%3)-m.poly_vert(f,off);
        vec3d b=m.poly_vert(f,(off+2)%3)-m.poly_vert(f,off);
        K-=std::atan2(a.cross(b).norm(),a.dot(b));
    }
    return K;
}

//::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::

vec3d face_normal(const Trimesh<> & m, const uint f)
{
    vec3d n=(m.poly_vert(f,1)-m.poly_vert(f,0)).cross(m.poly_vert(f,2)-m.poly_vert(f,0));
    n.normalize();
    return n;
}

//::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::

double mod_2pi(const double x)
{
    double r=std::fmod(x,2*PI);
    if(r> PI) r-=2*PI;
    if(r<-PI) r+=2*PI;
    return r;
}

//::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::

double mod_halfpi(const double x) // centered to (-pi/4,pi/4]
{
    return x-(PI/2)*std::floor(x/(PI/2)+0.5);
}

//::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::

// per vertex exact constraint: sum of beta around the positive ring == q*pi/2 - K
double vertex_constraint_residual(const Trimesh<> & m, const SingularityField & sf,
                                  const std::vector<int> & q, const uint v)
{
    std::vector<uint> fs,es;
    positive_ring(m,v,fs,es);
    double sb=0;
    for(size_t i=0;i<es.size();++i)
        sb+=sf.correction(es[i],fs[i],fs[(i+1)%fs.size()]);
    return sb-(q[v]*PI/2-angle_defect(m,v));
}

//::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::

// signed sum of a per edge quantity along basis cycle j
template<typename F>
double cycle_sum(const SingularityField & sf, const uint j, F && value)
{
    const SingularityField::Cycle & c=sf.cycles()[j];
    double s=0;
    for(size_t i=0;i<c.edges.size();++i) s+=c.signs[i]*value(c.edges[i]);
    return s;
}

//::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::

// independently built dense constraint system and KKT solve for beta optimality
void dense_min_norm_beta(const Trimesh<> & m, const SingularityField & sf,
                         const std::vector<int> & q, const std::vector<int> & periods,
                         Eigen::VectorXd & beta_out)
{
    uint V=m.num_verts(), E=m.num_edges(), G=uint(sf.cycles().size());
    uint rows=(V-1)+G;
    Eigen::MatrixXd A=Eigen::MatrixXd::Zero(rows,E);
    Eigen::VectorXd b(rows);
    for(uint v=0;v+1<V;++v)
    {
        std::vector<uint> fs,es;
        positive_ring(m,v,fs,es);
        b[v]=q[v]*PI/2-angle_defect(m,v);
        for(size_t i=0;i<es.size();++i)
        {
            uint fc=fs[i], fn=fs[(i+1)%fs.size()];
            A(v,es[i])+=(fc<fn)?1.0:-1.0;
        }
    }
    for(uint j=0;j<G;++j)
    {
        double so=cycle_sum(sf,j,[&](uint e){return sf.connections()[e];});
        b[(V-1)+j]=periods[j]*PI/2-so;
        const SingularityField::Cycle & c=sf.cycles()[j];
        for(size_t i=0;i<c.edges.size();++i) A((V-1)+j,c.edges[i])+=c.signs[i];
    }
    Eigen::MatrixXd KKT=Eigen::MatrixXd::Zero(E+rows,E+rows);
    KKT.topLeftCorner(E,E)=Eigen::MatrixXd::Identity(E,E);
    KKT.topRightCorner(E,rows)=A.transpose();
    KKT.bottomLeftCorner(rows,E)=A;
    Eigen::VectorXd rhs(E+rows);
    rhs.setZero();
    rhs.tail(rows)=b;
    Eigen::VectorXd sol=KKT.fullPivLu().solve(rhs);
    beta_out=sol.head(E);
}

//::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::

// representative angle of the published field in a face frame
double field_angle(const SingularityField & sf, const uint f)
{
    vec3d d=sf.field().vec_at(int(f));
    return std::atan2(d.dot(sf.frames()[f].v),d.dot(sf.frames()[f].u));
}

//::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::

bool expect_invalid_mesh(const Trimesh<> & m)
{
    try { SingularityField sf(m); }
    catch(const std::invalid_argument &) { return true; }
    return false;
}

//::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::

bool expect_invalid_solve(SingularityField & sf, const std::vector<int> & q,
                          const std::vector<int> & p, const vec3d & a)
{
    try { sf.solve(q,p,a); }
    catch(const std::invalid_argument &) { return true; }
    return false;
}

//::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
// group: connection
//::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::

int frames_right_handed()
{
    for(uint mesh_id=0; mesh_id<2; ++mesh_id)
    {
        Trimesh<> m = (mesh_id==0) ? make_sphere(2) : make_torus(7,5);
        SingularityField sf(m);
        TASK_CHECK(sf.num_faces()==m.num_polys());
        TASK_CHECK(sf.frames().size()==m.num_polys());
        for(uint f=0;f<m.num_polys();++f)
        {
            const vec3d & u=sf.frames()[f].u;
            const vec3d & v=sf.frames()[f].v;
            vec3d n=face_normal(m,f); // independent, from the winding
            TASK_CHECK(std::abs(u.norm()-1)<1e-12);
            TASK_CHECK(std::abs(v.norm()-1)<1e-12);
            TASK_CHECK(std::abs(u.dot(v))<1e-12);
            // right handed: u x v equals the face normal by winding
            TASK_CHECK((u.cross(v)-n).norm()<1e-12);
            TASK_CHECK((sf.face_normal(f)-n).norm()<1e-12);
            // u lies in the face plane
            TASK_CHECK(std::abs(u.dot(n))<1e-12);
        }
    }
    return 0;
}

//::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::

int transport_unfolding()
{
    Trimesh<> m=make_torus(7,5);
    SingularityField sf(m);
    for(uint e=0;e<m.num_edges();++e)
    {
        auto ef=sf.edge_faces(e);
        uint f=ef.first, g=ef.second;
        TASK_CHECK(f<g);
        // independent unfolding of g's frame onto f's plane
        vec3d n0=face_normal(m,f), n1=face_normal(m,g);
        vec3d axis=m.edge_vert(e,1)-m.edge_vert(e,0);
        axis.normalize();
        double ca=std::max(-1.0,std::min(1.0,n1.dot(n0)));
        double sa=n1.cross(n0).dot(axis);
        vec3d u1=sf.frames()[g].u;
        vec3d unf=u1*ca+axis.cross(u1)*sa+axis*(axis.dot(u1))*(1-ca);
        double expected=-std::atan2(sf.frames()[f].u.cross(unf).dot(n0),
                                    sf.frames()[f].u.dot(unf));
        TASK_CHECK(std::abs(expected-sf.connections()[e])<1e-9);
        // principal value and antisymmetry
        TASK_CHECK(sf.connections()[e]>-PI-1e-12 && sf.connections()[e]<=PI+1e-12);
        TASK_CHECK(std::abs(sf.connection(e,f,g)+sf.connection(e,g,f))<1e-15);
        TASK_CHECK(std::abs(sf.connection(e,f,g)-sf.connections()[e])<1e-15);
    }
    return 0;
}

//::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::

int vertex_ring_holonomy()
{
    for(uint mesh_id=0; mesh_id<2; ++mesh_id)
    {
        Trimesh<> m = (mesh_id==0) ? make_sphere(2) : make_torus(7,5);
        SingularityField sf(m);
        for(uint v=0;v<m.num_verts();++v)
        {
            std::vector<uint> fs,es;
            positive_ring(m,v,fs,es);
            TASK_CHECK(fs.size()==m.adj_v2p(v).size());
            double s=0;
            for(size_t i=0;i<es.size();++i)
                s+=sf.connection(es[i],fs[i],fs[(i+1)%fs.size()]);
            // transport angles around the positive ring agree with K mod 2pi
            TASK_CHECK(std::abs(mod_2pi(s-angle_defect(m,v)))<1e-9);
        }
    }
    return 0;
}

//::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::

int cycles_structure()
{
    Trimesh<> m=make_torus(7,5);
    SingularityField sf(m);
    TASK_CHECK(sf.cycles().size()==2);
    TASK_CHECK(sf.genus()==1);
    uint gens[2];
    for(uint j=0;j<2;++j)
    {
        const SingularityField::Cycle & c=sf.cycles()[j];
        TASK_CHECK(c.edges.size()==c.signs.size());
        TASK_CHECK(c.edges.size()>=2);
        TASK_CHECK(c.edges[0]==c.generator_edge);
        TASK_CHECK(c.signs[0]==+1); // generator dual edge goes from lower to higher face id
        gens[j]=c.generator_edge;
        // the signed edge sequence is a closed loop of the dual graph
        for(size_t i=0;i<c.edges.size();++i)
        {
            size_t n=(i+1)%c.edges.size();
            auto p0=sf.edge_faces(c.edges[i]);
            auto p1=sf.edge_faces(c.edges[n]);
            uint to_i  =(c.signs[i]==+1)?p0.second:p0.first;
            uint from_n=(c.signs[n]==+1)?p1.first :p1.second;
            TASK_CHECK(to_i==from_n);
        }
        // the generator appears exactly once in its own cycle
        uint count=0;
        for(uint e : c.edges) count+=(e==c.generator_edge)?1:0;
        TASK_CHECK(count==1);
    }
    TASK_CHECK(gens[0]!=gens[1]);
    TASK_CHECK(gens[0]<gens[1]); // increasing generator order
    // sphere: no cycles
    {
        Trimesh<> s=make_sphere(1);
        SingularityField ss(s);
        TASK_CHECK(ss.cycles().empty());
    }
    return 0;
}

//::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::

int rigid_scale_invariance()
{
    Trimesh<> m=make_sphere(2);
    SingularityField sf(m);
    std::vector<int> q(m.num_verts(),0);
    q[3]=5; q[17]=3;
    vec3d anchor=sf.frames()[0].u;
    TASK_CHECK(sf.solve(q,{},anchor));

    // rotate by 63 degrees around (1,2,3), scale by 2.5, translate
    vec3d axis(1,2,3); axis.normalize();
    double ca=std::cos(63.0*PI/180.0), sa=std::sin(63.0*PI/180.0);
    auto rot=[&](const vec3d & v){ return v*ca+axis.cross(v)*sa+axis*(axis.dot(v))*(1-ca); };
    std::vector<vec3d> p2;
    for(uint v=0;v<m.num_verts();++v) p2.push_back(rot(m.vert(v))*2.5+vec3d(3,-2,1));
    std::vector<uint> f2;
    for(uint f=0;f<m.num_polys();++f)
        f2.insert(f2.end(),{m.poly_vert_id(f,0),m.poly_vert_id(f,1),m.poly_vert_id(f,2)});
    Trimesh<> m2(p2,f2);
    SingularityField sf2(m2);
    TASK_CHECK(sf2.solve(q,{},rot(anchor)));

    // corrections are invariant (genus 0: no cycle rows involve omega)
    double wb=0;
    for(uint e=0;e<m.num_edges();++e) wb=std::max(wb,std::abs(sf.corrections()[e]-sf2.corrections()[e]));
    TASK_CHECK(wb<1e-9);
    // the represented four-direction field rotates with the mesh
    double wd=0;
    for(uint f=0;f<m.num_polys();++f)
        wd=std::max(wd,(rot(sf.field().vec_at(int(f)))-sf2.field().vec_at(int(f))).norm());
    TASK_CHECK(wd<1e-8);
    return 0;
}

//::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
// group: singularities
//::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::

int sphere_two_singularities()
{
    Trimesh<> m=make_sphere(2);
    SingularityField sf(m);
    std::vector<int> q(m.num_verts(),0);
    q[3]=5; q[17]=3; // sum = 8 = 4*chi
    TASK_CHECK(sf.solve(q,{},sf.frames()[0].u));

    // diagnostics are consistent and sparse factorization happened once
    const SingularityField::Diagnostics & d=sf.diagnostics();
    TASK_CHECK(d.max_constraint_residual<=1e-8);
    TASK_CHECK(d.dropped_vertex_residual<=1e-8);
    TASK_CHECK(d.sparse_factorizations==1);
    TASK_CHECK(d.energy>0);
    double energy=0;
    for(double b : sf.corrections()) energy+=b*b/2;
    TASK_CHECK(std::abs(energy-d.energy)<1e-10);

    // exact singularity constraints on ALL vertices (dropped one included),
    // from an independent unfolding of each positive vertex ring
    for(uint v=0;v<m.num_verts();++v)
        TASK_CHECK(std::abs(vertex_constraint_residual(m,sf,q,v))<=1e-8);

    // integer matching relation on every directed dual edge
    for(uint e=0;e<m.num_edges();++e)
    {
        auto ef=sf.edge_faces(e);
        double t0=field_angle(sf,ef.first), t1=field_angle(sf,ef.second);
        double r=t1-t0-sf.connections()[e]-sf.corrections()[e]-sf.quarter_turns()[e]*PI/2;
        TASK_CHECK(std::abs(mod_2pi(r))<1e-8);
        TASK_CHECK(sf.quarter_turn(e,ef.first,ef.second)==-sf.quarter_turn(e,ef.second,ef.first));
    }

    // representatives are unit and tangent
    for(uint f=0;f<m.num_polys();++f)
    {
        vec3d dir=sf.field().vec_at(int(f));
        TASK_CHECK(std::abs(dir.norm()-1)<1e-9);
        TASK_CHECK(std::abs(dir.dot(face_normal(m,f)))<1e-9);
    }
    return 0;
}

//::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::

int sphere_eight_singularities()
{
    Trimesh<> m=make_sphere(2);
    SingularityField sf(m);
    std::vector<int> q(m.num_verts(),0);
    uint ids[8]={2,9,15,23,31,44,52,65};
    for(uint v : ids) q[v]=1; // eight +1/4 singularities
    TASK_CHECK(sf.solve(q,{},sf.frames()[0].u));

    // the singularity index measured from the published field alone
    // (continuous lift of the representative along each positive ring,
    // plus the angle defect) equals q[v]/4 at every vertex
    for(uint v=0;v<m.num_verts();++v)
    {
        std::vector<uint> fs,es;
        positive_ring(m,v,fs,es);
        double lift=0;
        for(size_t i=0;i<fs.size();++i)
        {
            uint fc=fs[i], fn=fs[(i+1)%fs.size()];
            double jump=field_angle(sf,fn)-field_angle(sf,fc)-sf.connection(es[i],fc,fn);
            lift+=mod_halfpi(jump);
        }
        double idx4=(lift+angle_defect(m,v))/(PI/2); // in units of pi/2
        TASK_CHECK(std::abs(idx4-q[v])<1e-6);
    }
    return 0;
}

//::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::

int beta_optimality_dense()
{
    // small sphere
    {
        Trimesh<> m=make_sphere(2);
        SingularityField sf(m);
        std::vector<int> q(m.num_verts(),0);
        q[3]=5; q[17]=3;
        TASK_CHECK(sf.solve(q,{},sf.frames()[0].u));
        Eigen::VectorXd dense;
        dense_min_norm_beta(m,sf,q,{},dense);
        double w=0;
        for(uint e=0;e<m.num_edges();++e) w=std::max(w,std::abs(dense[e]-sf.corrections()[e]));
        TASK_CHECK(w<1e-8);
    }
    // small torus with non zero periods
    {
        Trimesh<> m=make_torus(7,5);
        SingularityField sf(m);
        std::vector<int> q(m.num_verts(),0);
        std::vector<int> periods={1,-2};
        TASK_CHECK(sf.solve(q,periods,sf.frames()[0].u));
        Eigen::VectorXd dense;
        dense_min_norm_beta(m,sf,q,periods,dense);
        double w=0;
        for(uint e=0;e<m.num_edges();++e) w=std::max(w,std::abs(dense[e]-sf.corrections()[e]));
        TASK_CHECK(w<1e-8);
    }
    return 0;
}

//::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::

int anchor_phase()
{
    Trimesh<> m=make_sphere(2);
    SingularityField sf(m);
    std::vector<int> q(m.num_verts(),0);
    q[3]=5; q[17]=3;
    vec3d a1=sf.frames()[0].u;
    TASK_CHECK(sf.solve(q,{},a1));
    VectorField f1=sf.field();
    std::vector<double> b1=sf.corrections();

    // rotating the anchor by 0.7 rotates every representative by 0.7 (mod pi/2)
    vec3d a2=sf.frames()[0].u*std::cos(0.7)+sf.frames()[0].v*std::sin(0.7);
    TASK_CHECK(sf.solve(q,{},a2));
    double wb=0;
    for(uint e=0;e<m.num_edges();++e) wb=std::max(wb,std::abs(b1[e]-sf.corrections()[e]));
    TASK_CHECK(wb==0.0); // beta does not depend on the anchor
    for(uint f=0;f<m.num_polys();++f)
    {
        double t1=std::atan2(f1.vec_at(int(f)).dot(sf.frames()[f].v),f1.vec_at(int(f)).dot(sf.frames()[f].u));
        double t2=field_angle(sf,f);
        TASK_CHECK(std::abs(mod_halfpi(t2-t1-0.7))<1e-9);
    }

    // rotating the anchor by pi/2 yields perpendicular representatives (same cross)
    vec3d a3=sf.frames()[0].v;
    TASK_CHECK(sf.solve(q,{},a3));
    for(uint f=0;f<m.num_polys();++f)
        TASK_CHECK(std::abs(f1.vec_at(int(f)).dot(sf.field().vec_at(int(f))))<1e-9);
    return 0;
}

//::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::

int invalid_input_recovery()
{
    Trimesh<> m=make_sphere(1);
    Trimesh<> t=make_torus(7,5);

    // queries before the first successful solve
    {
        SingularityField sf(m);
        bool logic=false;
        try { sf.field(); }        catch(const std::logic_error &) { logic=true; }
        TASK_CHECK(logic);
        logic=false;
        try { sf.corrections(); }  catch(const std::logic_error &) { logic=true; }
        TASK_CHECK(logic);
        logic=false;
        try { sf.quarter_turns(); }catch(const std::logic_error &) { logic=true; }
        TASK_CHECK(logic);
        logic=false;
        try { sf.diagnostics(); }  catch(const std::logic_error &) { logic=true; }
        TASK_CHECK(logic);
        // geometry queries are available instead
        TASK_CHECK(sf.frames().size()==m.num_polys());
        TASK_CHECK(sf.connections().size()==m.num_edges());
        TASK_CHECK(sf.cycles().empty());
    }

    // invalid meshes
    TASK_CHECK(expect_invalid_mesh(Trimesh<>())); // empty
    {
        std::vector<vec3d> p={vec3d(0,0,0),vec3d(1,0,0),vec3d(0,1,0),vec3d(0,0,1)};
        p[1].x()=std::nan("");
        TASK_CHECK(expect_invalid_mesh(Trimesh<>(p,std::vector<uint>{0,2,1,0,1,3,1,2,3,2,0,3}))); // NaN
    }
    {
        std::vector<vec3d> p={vec3d(0,0,0),vec3d(1,0,0),vec3d(0,1,0),vec3d(0,0,1)};
        p[2].y()=std::numeric_limits<double>::infinity();
        TASK_CHECK(expect_invalid_mesh(Trimesh<>(p,std::vector<uint>{0,2,1,0,1,3,1,2,3,2,0,3}))); // inf
    }
    {
        std::vector<vec3d> p={vec3d(0,0,0),vec3d(1,0,0),vec3d(0,1,0),vec3d(0.5,0,0)};
        TASK_CHECK(expect_invalid_mesh(Trimesh<>(p,std::vector<uint>{0,2,1,0,1,3,1,2,3,2,0,3}))); // degenerate face
    }
    {
        std::vector<vec3d> p={vec3d(0,0,0),vec3d(1,0,0),vec3d(0,1,0),vec3d(0,0,1)};
        TASK_CHECK(expect_invalid_mesh(Trimesh<>(p,std::vector<uint>{0,2,1,0,1,3,2,0,3}))); // open
    }
    {
        std::vector<vec3d> p={vec3d(0,0,0),vec3d(1,0,0),vec3d(0,1,0),vec3d(0,0,1)};
        TASK_CHECK(expect_invalid_mesh(Trimesh<>(p,std::vector<uint>{0,2,1,0,1,3,1,3,2,2,0,3}))); // flipped face
    }
    {
        // two tetrahedra: disconnected
        std::vector<vec3d> p={vec3d(0,0,0),vec3d(1,0,0),vec3d(0,1,0),vec3d(0,0,1),
                              vec3d(3,0,0),vec3d(4,0,0),vec3d(3,1,0),vec3d(3,0,1)};
        std::vector<uint> f={0,2,1,0,1,3,1,2,3,2,0,3, 4,6,5,4,5,7,5,6,7,6,4,7};
        TASK_CHECK(expect_invalid_mesh(Trimesh<>(p,f)));
    }
    {
        // two tetrahedra sharing one vertex: non manifold vertex
        std::vector<vec3d> p={vec3d(0,0,0),vec3d(1,0,0),vec3d(0,1,0),vec3d(0,0,1),
                              vec3d(1,1,0),vec3d(0,1,1),vec3d(-1,0,0)};
        std::vector<uint> f={0,2,1,0,1,3,1,2,3,2,0,3, 0,3,4,0,4,5,0,5,6,0,6,3};
        TASK_CHECK(expect_invalid_mesh(Trimesh<>(p,f)));
    }
    {
        // two tetrahedra sharing one edge: non manifold edge
        std::vector<vec3d> p={vec3d(0,0,0),vec3d(1,0,0),vec3d(0,1,0),vec3d(0,0,1),
                              vec3d(1,1,1),vec3d(0,-1,0)};
        std::vector<uint> f={0,2,1,0,1,3,1,2,3,2,0,3, 0,1,4,1,5,4,0,4,5,0,5,1};
        TASK_CHECK(expect_invalid_mesh(Trimesh<>(p,f)));
    }
    {
        // isolated vertex
        std::vector<vec3d> p={vec3d(0,0,0),vec3d(1,0,0),vec3d(0,1,0),vec3d(0,0,1),vec3d(5,5,5)};
        TASK_CHECK(expect_invalid_mesh(Trimesh<>(p,std::vector<uint>{0,2,1,0,1,3,1,2,3,2,0,3})));
    }
    {
        // genus 2
        Trimesh<> g2=make_double_torus();
        TASK_CHECK(g2.genus()==2);
        bool oriented=true;
        for(uint e=0;e<g2.num_edges();++e)
        {
            if(g2.adj_e2p(e).size()!=2 ||
               g2.edge_is_CCW(e,g2.adj_e2p(e).at(0))==g2.edge_is_CCW(e,g2.adj_e2p(e).at(1)))
                oriented=false;
        }
        TASK_CHECK(oriented); // the double torus is a valid oriented closed mesh, only the genus is wrong
        TASK_CHECK(expect_invalid_mesh(g2));
    }

    // a valid object keeps its last successful state after invalid solve inputs
    SingularityField sf(t);
    std::vector<int> q(t.num_verts(),0);
    std::vector<int> periods={1,0};
    vec3d anchor=sf.frames()[0].u;
    TASK_CHECK(sf.solve(q,periods,anchor));
    VectorField      f0=sf.field();
    std::vector<double> b0=sf.corrections();
    double           e0=sf.diagnostics().energy;

    TASK_CHECK(expect_invalid_solve(sf,std::vector<int>(t.num_verts()+1,0),periods,anchor)); // bad q size
    TASK_CHECK(expect_invalid_solve(sf,std::vector<int>(t.num_verts(),1),periods,anchor));   // bad q sum
    TASK_CHECK(expect_invalid_solve(sf,q,std::vector<int>{1},anchor));                       // bad periods size
    TASK_CHECK(expect_invalid_solve(sf,q,periods,vec3d(0,0,0)));                             // zero anchor
    TASK_CHECK(expect_invalid_solve(sf,q,periods,vec3d(std::nan(""),0,0)));                  // NaN anchor
    TASK_CHECK(expect_invalid_solve(sf,q,periods,anchor+sf.face_normal(0)));                 // non tangent anchor

    // state preserved
    TASK_CHECK((sf.field()-f0).norm()==0.0);
    TASK_CHECK(sf.corrections()==b0);
    TASK_CHECK(sf.diagnostics().energy==e0);

    // and a valid solve still works afterwards
    TASK_CHECK(sf.solve(q,std::vector<int>{0,0},anchor));
    TASK_CHECK(sf.diagnostics().max_constraint_residual<=1e-8);

    // directed accessors validate their arguments
    {
        auto ef=sf.edge_faces(0);
        bool bad=false;
        try { sf.connection(0,ef.first,ef.first); }
        catch(const std::invalid_argument &) { bad=true; }
        TASK_CHECK(bad);
    }
    return 0;
}

//::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::

int target_ABA()
{
    Trimesh<> m=make_sphere(2);
    SingularityField sf(m);
    std::vector<int> qA(m.num_verts(),0), qB(m.num_verts(),0);
    qA[3]=5; qA[17]=3;
    qB[9]=2; qB[23]=4; qB[40]=2;
    vec3d aA=sf.frames()[0].u;
    vec3d aB=sf.frames()[0].u*std::cos(0.4)+sf.frames()[0].v*std::sin(0.4);

    TASK_CHECK(sf.solve(qA,{},aA));
    VectorField fA=sf.field();
    std::vector<double> bA=sf.corrections();
    std::vector<int>    tA=sf.quarter_turns();

    TASK_CHECK(sf.solve(qB,{},aB));
    double wb=0;
    for(uint e=0;e<m.num_edges();++e) wb=std::max(wb,std::abs(bA[e]-sf.corrections()[e]));
    TASK_CHECK(wb>1e-3); // target B is genuinely different

    TASK_CHECK(sf.solve(qA,{},aA));
    TASK_CHECK((sf.field()-fA).norm()<1e-12);
    TASK_CHECK(sf.corrections()==bA);
    TASK_CHECK(sf.quarter_turns()==tA);
    return 0;
}

//::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
// group: periods
//::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::

int handle_periods()
{
    Trimesh<> m=make_torus(9,7);
    SingularityField sf(m);
    std::vector<int> q(m.num_verts(),0);
    const int P[4][2]={{0,0},{1,0},{0,1},{2,-1}};
    VectorField fields[4];
    for(uint k=0;k<4;++k)
    {
        std::vector<int> periods={P[k][0],P[k][1]};
        TASK_CHECK(sf.solve(q,periods,sf.frames()[0].u));
        fields[k]=sf.field();
        // each basis cycle picks up exactly periods[j] quarter turns
        for(uint j=0;j<2;++j)
        {
            double sb=cycle_sum(sf,j,[&](uint e){return sf.corrections()[e];});
            double so=cycle_sum(sf,j,[&](uint e){return sf.connections()[e];});
            TASK_CHECK(std::abs(sb-(periods[j]*PI/2-so))<=1e-8);
        }
        // vertex constraints remain satisfied (q=0)
        double w=0;
        for(uint v=0;v<m.num_verts();++v)
            w=std::max(w,std::abs(vertex_constraint_residual(m,sf,q,v)));
        TASK_CHECK(w<=1e-8);
        TASK_CHECK(sf.diagnostics().max_constraint_residual<=1e-8);
    }
    // different period vectors give genuinely different fields (mod pi/2)
    for(uint a=0;a<4;++a) for(uint b=a+1;b<4;++b)
    {
        double worst=0;
        for(uint f=0;f<m.num_polys();++f)
        {
            double ta=std::atan2(fields[a].vec_at(int(f)).dot(sf.frames()[f].v),
                                 fields[a].vec_at(int(f)).dot(sf.frames()[f].u));
            double tb=std::atan2(fields[b].vec_at(int(f)).dot(sf.frames()[f].v),
                                 fields[b].vec_at(int(f)).dot(sf.frames()[f].u));
            worst=std::max(worst,std::abs(mod_halfpi(ta-tb)));
        }
        TASK_CHECK(worst>1e-3);
    }
    return 0;
}

//::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::

int cycle_quarter_turns()
{
    Trimesh<> m=make_torus(9,7);
    SingularityField sf(m);
    std::vector<int> q(m.num_verts(),0);
    std::vector<int> periods={2,-1};
    TASK_CHECK(sf.solve(q,periods,sf.frames()[0].u));

    // the integer matching winds minus the prescribed periods around each handle
    for(uint j=0;j<2;++j)
    {
        long sq=std::lround(cycle_sum(sf,j,[&](uint e){return double(sf.quarter_turns()[e]);}));
        TASK_CHECK(sq+periods[j]==0);
    }
    // and the corrected connection integrates to the periods exactly
    for(uint j=0;j<2;++j)
    {
        double sw=cycle_sum(sf,j,[&](uint e){return sf.connections()[e]+sf.corrections()[e];});
        TASK_CHECK(std::abs(sw-periods[j]*PI/2)<=1e-8);
    }
    // integer matching relation on every edge
    for(uint e=0;e<m.num_edges();++e)
    {
        auto ef=sf.edge_faces(e);
        double r=field_angle(sf,ef.second)-field_angle(sf,ef.first)
                -sf.connections()[e]-sf.corrections()[e]-sf.quarter_turns()[e]*PI/2;
        TASK_CHECK(std::abs(mod_2pi(r))<1e-8);
    }
    return 0;
}

//::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::

int field_winding_around_handles()
{
    Trimesh<> m=make_torus(9,7);
    SingularityField sf(m);
    std::vector<int> q(m.num_verts(),0);
    std::vector<int> periods={2,-1};
    TASK_CHECK(sf.solve(q,periods,sf.frames()[0].u));

    // Levi-Civita transport of the published representative around each basis
    // cycle accumulates, mod pi/2, the prescribed winding minus the cycle's
    // Levi-Civita holonomy (independently unfolded here from the field alone)
    for(uint j=0;j<2;++j)
    {
        const SingularityField::Cycle & c=sf.cycles()[j];
        double lift=0, so=0;
        for(size_t i=0;i<c.edges.size();++i)
        {
            auto ef=sf.edge_faces(c.edges[i]);
            uint from=(c.signs[i]==+1)?ef.first :ef.second;
            uint to  =(c.signs[i]==+1)?ef.second:ef.first;
            lift+=mod_halfpi(field_angle(sf,to)-field_angle(sf,from)
                            -sf.connection(c.edges[i],from,to));
            so  +=c.signs[i]*sf.connections()[c.edges[i]];
        }
        double expected=periods[j]*PI/2-so; // == sum of beta along the cycle
        TASK_CHECK(std::abs(mod_halfpi(lift-expected))<1e-8);
    }
    return 0;
}

//::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
// group: reuse
//::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::

int big_torus_reuse()
{
    Trimesh<> m=make_torus(32,20); // 1280 faces
    TASK_CHECK(m.num_polys()>1000);
    SingularityField sf(m);
    std::vector<int> q(m.num_verts(),0);

    const int P[4][2]={{0,0},{1,0},{0,2},{-1,1}};
    for(uint k=0;k<4;++k)
    {
        vec3d anchor=sf.frames()[0].u*std::cos(0.3*k)+sf.frames()[0].v*std::sin(0.3*k);
        TASK_CHECK(sf.solve(q,{P[k][0],P[k][1]},anchor));
        // one single sparse factorization, reused for every solve
        TASK_CHECK(sf.diagnostics().sparse_factorizations==1);
        // sparse storage of the normal equations
        uint rows=sf.diagnostics().matrix_rows;
        TASK_CHECK(rows==m.num_verts()-1+2);
        TASK_CHECK(sf.diagnostics().matrix_nonzeros<20*rows);
        // independent residual recomputation (not just reading diagnostics)
        double w=0;
        for(uint v=0;v<m.num_verts();++v)
            w=std::max(w,std::abs(vertex_constraint_residual(m,sf,q,v)));
        TASK_CHECK(w<=1e-8);
        for(uint j=0;j<2;++j)
        {
            double sb=cycle_sum(sf,j,[&](uint e){return sf.corrections()[e];});
            double so=cycle_sum(sf,j,[&](uint e){return sf.connections()[e];});
            TASK_CHECK(std::abs(sb-(P[k][j]*PI/2-so))<=1e-8);
        }
        TASK_CHECK(sf.diagnostics().max_constraint_residual<=1e-8);
        double energy=0;
        for(double b : sf.corrections()) energy+=b*b/2;
        TASK_CHECK(std::abs(energy-sf.diagnostics().energy)<1e-10);
    }
    TASK_CHECK(sf.diagnostics().sparse_factorizations==1);
    return 0;
}

//::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::

int big_sphere_reuse()
{
    Trimesh<> m=make_sphere(4); // 2048 faces
    TASK_CHECK(m.num_polys()>1000);
    SingularityField sf(m);
    std::vector<int> q(m.num_verts(),0);
    uint ids[8]={2,9,15,23,31,44,52,65};
    for(uint v : ids) q[v]=1;

    for(uint k=0;k<3;++k)
    {
        vec3d anchor=sf.frames()[0].u*std::cos(0.5*k)+sf.frames()[0].v*std::sin(0.5*k);
        TASK_CHECK(sf.solve(q,{},anchor));
        TASK_CHECK(sf.diagnostics().sparse_factorizations==1);
        uint rows=sf.diagnostics().matrix_rows;
        TASK_CHECK(rows==m.num_verts()-1);
        TASK_CHECK(sf.diagnostics().matrix_nonzeros<20*rows);
        double w=0;
        for(uint v=0;v<m.num_verts();++v)
            w=std::max(w,std::abs(vertex_constraint_residual(m,sf,q,v)));
        TASK_CHECK(w<=1e-8);
        TASK_CHECK(sf.diagnostics().max_constraint_residual<=1e-8);
    }
    return 0;
}

//::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::

int source_mesh_independence()
{
    Trimesh<> keep=make_torus(7,5); // the test's own reference copy
    SingularityField *sf;
    {
        Trimesh<> m=make_torus(7,5);
        // the source mesh is not modified by the constructor
        std::vector<vec3d> verts_before=m.vector_verts();
        std::vector<uint>  polys_before;
        for(uint f=0;f<m.num_polys();++f)
            polys_before.insert(polys_before.end(),{m.poly_vert_id(f,0),m.poly_vert_id(f,1),m.poly_vert_id(f,2)});
        uint E_before=m.num_edges();
        sf=new SingularityField(m);
        TASK_CHECK(m.vector_verts()==verts_before);
        TASK_CHECK(m.num_edges()==E_before);
        for(uint f=0;f<m.num_polys();++f)
        {
            TASK_CHECK(m.poly_vert_id(f,0)==polys_before[3*f  ]);
            TASK_CHECK(m.poly_vert_id(f,1)==polys_before[3*f+1]);
            TASK_CHECK(m.poly_vert_id(f,2)==polys_before[3*f+2]);
        }
        // mutate and then destroy the source mesh
        m.vert(0)=vec3d(1e6,1e6,1e6);
    }
    // the object owns its snapshot: solves are unaffected
    std::vector<int> q(keep.num_verts(),0);
    std::vector<int> periods={1,-1};
    TASK_CHECK(sf->solve(q,periods,sf->frames()[0].u));
    double w=0;
    for(uint v=0;v<keep.num_verts();++v)
        w=std::max(w,std::abs(vertex_constraint_residual(keep,*sf,q,v)));
    TASK_CHECK(w<=1e-8);
    for(uint j=0;j<2;++j)
    {
        double sb=cycle_sum(*sf,j,[&](uint e){return sf->corrections()[e];});
        double so=cycle_sum(*sf,j,[&](uint e){return sf->connections()[e];});
        TASK_CHECK(std::abs(sb-(periods[j]*PI/2-so))<=1e-8);
    }
    delete sf;
    return 0;
}

//::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::

int repeated_solve_deterministic()
{
    Trimesh<> m=make_torus(9,7);
    SingularityField sf(m);
    std::vector<int> q(m.num_verts(),0);
    std::vector<int> periods={1,-2};
    vec3d anchor=sf.frames()[0].u;
    TASK_CHECK(sf.solve(q,periods,anchor));
    VectorField f1=sf.field();
    std::vector<double> b1=sf.corrections();
    TASK_CHECK(sf.solve(q,periods,anchor));
    TASK_CHECK((sf.field()-f1).norm()==0.0);
    TASK_CHECK(sf.corrections()==b1);
    TASK_CHECK(sf.diagnostics().sparse_factorizations==1);
    return 0;
}

} // namespace

//::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::

const struct task_case task_cases[] =
{
    { "connection",    "frames_right_handed",       &frames_right_handed       },
    { "connection",    "transport_unfolding",       &transport_unfolding       },
    { "connection",    "vertex_ring_holonomy",      &vertex_ring_holonomy      },
    { "connection",    "cycles_structure",          &cycles_structure          },
    { "connection",    "rigid_scale_invariance",    &rigid_scale_invariance    },
    { "singularities", "sphere_two_singularities",  &sphere_two_singularities  },
    { "singularities", "sphere_eight_singularities",&sphere_eight_singularities},
    { "singularities", "beta_optimality_dense",     &beta_optimality_dense     },
    { "singularities", "anchor_phase",              &anchor_phase              },
    { "singularities", "invalid_input_recovery",    &invalid_input_recovery    },
    { "singularities", "target_ABA",                &target_ABA                },
    { "periods",       "handle_periods",            &handle_periods            },
    { "periods",       "cycle_quarter_turns",       &cycle_quarter_turns       },
    { "periods",       "field_winding_around_handles",&field_winding_around_handles },
    { "reuse",         "big_torus_reuse",           &big_torus_reuse           },
    { "reuse",         "big_sphere_reuse",          &big_sphere_reuse          },
    { "reuse",         "source_mesh_independence",  &source_mesh_independence  },
    { "reuse",         "repeated_solve_deterministic",&repeated_solve_deterministic },
};

const size_t task_case_count = sizeof(task_cases)/sizeof(task_cases[0]);
