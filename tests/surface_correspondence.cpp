#include "../task_support/check.h"

#include <cinolib/surface_correspondence.h>

#include <array>
#include <cmath>
#include <map>
#include <random>
#include <stdexcept>
#include <vector>

using namespace cinolib;

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace
{

constexpr double EPS = 1e-9;

//::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
// mesh builders
//::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::

// asymmetric, non symmetric flat disk with a non uniform boundary
Trimesh<> flat_disk(unsigned nx, unsigned ny)
{
    std::vector<vec3d> v;
    std::vector<uint> f;
    for(unsigned y=0;y<ny;++y)
        for(unsigned x=0;x<nx;++x)
            v.emplace_back(double(x), double(y), 0.02*double(x*x + 2*y*y));
    for(unsigned y=0;y+1<ny;++y)
        for(unsigned x=0;x+1<nx;++x)
        {
            uint a = x + nx*y;
            f.insert(f.end(), {a, a+1, a+nx+1, a, a+nx+1, a+nx});
        }
    return Trimesh<>(v, f);
}

Trimesh<> bent_patch(int n)
{
    std::vector<vec3d> v;
    std::vector<uint> f;
    for(int y=0;y<n;++y)
        for(int x=0;x<n;++x)
            v.emplace_back(x, y, 0.35*std::sin(0.8*x) + 0.22*std::cos(0.9*y));
    for(int y=0;y+1<n;++y)
        for(int x=0;x+1<n;++x)
        {
            uint a = uint(x + n*y);
            f.insert(f.end(), {a, a+1, a+uint(n)+1, a, a+uint(n)+1, a+uint(n)});
        }
    return Trimesh<>(v, f);
}

// welded subdivided icosahedron (the bundled icosphere() does not weld midpoints)
Trimesh<> welded_sphere(int subd)
{
    const double A = 0.5257310271, B = 0.8506510258;
    std::vector<vec3d> V;
    for(auto p : std::vector<std::array<double,3>>{
        {-A,0,-B},{ A,0,-B},{ A,0, B},{-A,0, B},
        {-B,-A,0},{-B, A,0},{ B, A,0},{ B,-A,0},
        {0,-B, A},{0,-B,-A},{0, B,-A},{0, B, A}})
        V.emplace_back(p[0],p[1],p[2]);
    std::vector<std::array<uint,3>> T = {
        {1,9,0},{10,1,0},{5,10,0},{4,5,0},{9,4,0},
        {8,2,3},{4,8,3},{5,4,3},{11,5,3},{2,11,3},
        {11,2,6},{10,11,6},{1,10,6},{7,1,6},{2,7,6},
        {11,10,5},{9,8,4},{7,2,8},{9,7,8},{1,7,9}};
    for(int s=0;s<subd;++s)
    {
        std::map<std::pair<uint,uint>,uint> mid;
        auto midpoint = [&](uint a, uint b) -> uint
        {
            auto key = std::make_pair(std::min(a,b), std::max(a,b));
            auto it = mid.find(key);
            if(it != mid.end()) return it->second;
            vec3d p = V[a] + V[b];
            p.normalize();
            uint id = uint(V.size());
            V.push_back(p);
            mid[key] = id;
            return id;
        };
        std::vector<std::array<uint,3>> nt;
        for(auto & t : T)
        {
            uint a=t[0], b=t[1], c=t[2];
            uint ab=midpoint(a,b), bc=midpoint(b,c), ca=midpoint(c,a);
            nt.push_back({a,ab,ca}); nt.push_back({ab,b,bc});
            nt.push_back({ca,bc,c});  nt.push_back({ab,bc,ca});
        }
        T.swap(nt);
    }
    std::vector<uint> ser;
    for(auto & t : T) ser.insert(ser.end(), t.begin(), t.end());
    return Trimesh<>(V, ser);
}

// annular patch with three rings: it has interior (non boundary) vertices
Trimesh<> holed_patch()
{
    const int N = 24;
    std::vector<vec3d> v;
    auto ring = [&](double r, double z, double phase){
        for(int i=0;i<N;++i)
        {
            double a = 2.0*M_PI*(i+phase)/N;
            v.emplace_back(r*std::cos(a), r*std::sin(a), z);
        }
    };
    ring(4.0, 0.0, 0.0);          // outer boundary
    ring(2.6, 0.15, 0.5);         // middle (interior) ring
    ring(1.5, 0.30, 0.0);         // hole boundary
    std::vector<uint> f;
    auto band = [&](uint r0, uint r1){
        for(int i=0;i<N;++i)
        {
            uint a0 = r0+uint(i),       a1 = r0+uint((i+1)%N);
            uint b0 = r1+uint(i),       b1 = r1+uint((i+1)%N);
            f.insert(f.end(), {a0,a1,b1, a0,b1,b0});
        }
    };
    band(0, N);
    band(N, 2*N);
    return Trimesh<>(v, f);
}

// two planar grids sharing the same footprint, separated by a tiny z gap
Trimesh<> two_close_shells()
{
    std::vector<vec3d> v;
    std::vector<uint> f;
    auto shell = [&](double z0, uint base){
        const int n = 5;
        for(int y=0;y<n;++y)
            for(int x=0;x<n;++x)
                v.emplace_back(x, y, z0 + 0.01*x);
        for(int y=0;y+1<n;++y)
            for(int x=0;x+1<n;++x)
            {
                uint a = base + uint(x + n*y);
                f.insert(f.end(), {a, a+1, a+uint(n)+1, a, a+uint(n)+1, a+uint(n)});
            }
    };
    shell(0.0, 0);
    shell(0.02, 25);
    return Trimesh<>(v, f);
}

// repeatedly collapse the first admissible non boundary edge
int run_collapses(SurfaceCorrespondence & sc, int target, double lambda = 0.4)
{
    int done = 0;
    for(int s=0; s<target; ++s)
    {
        bool ok = false;
        for(uint e=0; e<sc.mesh().num_edges(); ++e)
        {
            if(sc.mesh().edge_is_incident_to_boundary(e)) continue;
            if(sc.collapse(e, lambda)) { ok = true; break; }
        }
        if(!ok) break;
        ++done;
    }
    return done;
}

double tri_area_3d(const std::array<vec3d,3> & t)
{
    return 0.5 * (t[1]-t[0]).cross(t[2]-t[0]).norm();
}

bool valid_bary(const std::array<double,3> & w)
{
    double s = 0.0;
    for(double x : w)
    {
        if(!std::isfinite(x) || x < -1e-12) return false;
        s += x;
    }
    return std::fabs(s-1.0) <= 1e-12;
}

// independent per-face area coverage check on both sides
bool check_coverage(const SurfaceCorrespondence & sc, const Trimesh<> & src)
{
    std::vector<double> sa(sc.num_source_faces(), 0.0);
    std::vector<double> ca(sc.mesh().num_polys(), 0.0);
    for(const CorrespondencePatch & pa : sc.patches())
    {
        for(int k=0;k<3;++k)
        {
            if(!valid_bary(pa.sourceBary[k]))  return false;
            if(!valid_bary(pa.currentBary[k])) return false;
        }
        auto sv = src.poly_verts_id(pa.sourceFace);
        std::array<vec3d,3> st{src.vert(sv[0]), src.vert(sv[1]), src.vert(sv[2])};
        std::array<vec3d,3> sc3;
        for(int k=0;k<3;++k)
            sc3[k] = pa.sourceBary[k][0]*st[0] + pa.sourceBary[k][1]*st[1] + pa.sourceBary[k][2]*st[2];
        double as = tri_area_3d(sc3);
        if(as <= 0.0) return false;
        sa[pa.sourceFace] += as;

        const Trimesh<> & m = sc.mesh();
        auto cv = m.poly_verts_id(pa.currentFace);
        std::array<vec3d,3> ct{m.vert(cv[0]), m.vert(cv[1]), m.vert(cv[2])};
        std::array<vec3d,3> cc3;
        for(int k=0;k<3;++k)
            cc3[k] = pa.currentBary[k][0]*ct[0] + pa.currentBary[k][1]*ct[1] + pa.currentBary[k][2]*ct[2];
        double ac = tri_area_3d(cc3);
        if(ac <= 0.0) return false;
        ca[pa.currentFace] += ac;
    }
    for(uint f=0; f<sc.num_source_faces(); ++f)
    {
        double ref = src.poly_area(f);
        if(sa[f] <= 0.0) return false;
        if(std::fabs(sa[f]-ref) > EPS*std::max(1.0, ref)) return false;
    }
    for(uint f=0; f<sc.mesh().num_polys(); ++f)
    {
        double ref = sc.mesh().poly_area(f);
        if(ca[f] <= 0.0) return false;
        if(std::fabs(ca[f]-ref) > EPS*std::max(1.0, ref)) return false;
    }
    return true;
}

// deterministic interior samples cover every source face
bool roundtrip_from_source(const SurfaceCorrespondence & sc, double & worst)
{
    std::mt19937 rng(987654321u);
    std::uniform_real_distribution<double> U(0.0, 1.0);
    worst = 0.0;
    for(uint sf=0; sf<sc.num_source_faces(); ++sf)
    {
        for(int k=0;k<5;++k)
        {
            double a=U(rng), b=U(rng);
            if(a+b>1.0){ a=1.0-a; b=1.0-b; }
            std::array<double,3> w{a, b, 1.0-a-b};
            vec3d p0 = sc.source_point(sf, w);
            SurfaceQuery q = sc.toCurrent(sf, w);
            SurfaceQuery r = sc.toSource(q.face, q.bary);
            vec3d p1 = sc.source_point(r.face, r.bary);
            worst = std::max(worst, (p0-p1).norm());
            if((p0-p1).norm() > EPS) return false;
        }
    }
    return true;
}

//::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
// maps group
//::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::

int maps_identity()
{
    Trimesh<> m = flat_disk(6, 5);
    SurfaceCorrespondence sc(m);
    TASK_CHECK(sc.num_source_faces() == m.num_polys());
    TASK_CHECK(sc.num_current_faces() == m.num_polys());
    TASK_CHECK(sc.patches().size() == m.num_polys());
    for(const CorrespondencePatch & pa : sc.patches())
    {
        TASK_CHECK(pa.sourceFace == pa.currentFace);
        for(int k=0;k<3;++k)
        {
            for(int j=0;j<3;++j)
            {
                double expect = (k==j) ? 1.0 : 0.0;
                TASK_CHECK(std::fabs(pa.sourceBary[k][j]-expect) < EPS);
                TASK_CHECK(std::fabs(pa.currentBary[k][j]-expect) < EPS);
            }
        }
    }
    return 0;
}

int maps_disk_coverage()
{
    Trimesh<> m = flat_disk(8, 7);
    SurfaceCorrespondence sc(m);
    int done = run_collapses(sc, 6);
    TASK_CHECK(done == 6);
    TASK_CHECK(sc.num_current_faces() == m.num_polys() - 2*done);
    TASK_CHECK(check_coverage(sc, m));
    // every current face is referenced by at least one patch and vice versa
    std::vector<char> seen(sc.mesh().num_polys(), 0);
    for(const CorrespondencePatch & pa : sc.patches())
    {
        TASK_CHECK(pa.currentFace < sc.mesh().num_polys());
        TASK_CHECK(pa.sourceFace  < sc.num_source_faces());
        seen[pa.currentFace] = 1;
    }
    for(char c : seen) TASK_CHECK(c);
    return 0;
}

int maps_sphere_orientation()
{
    Trimesh<> m = welded_sphere(2);
    SurfaceCorrespondence sc(m);
    int done = run_collapses(sc, 12, 0.5);
    TASK_CHECK(done >= 8);
    TASK_CHECK(check_coverage(sc, m));
    // every patch has strictly positive area on both sides (checked inside)
    TASK_CHECK(!sc.patches().empty());
    return 0;
}

int maps_bent_coverage()
{
    Trimesh<> m = bent_patch(7);
    SurfaceCorrespondence sc(m);
    int done = run_collapses(sc, 6);
    TASK_CHECK(done >= 4);
    TASK_CHECK(check_coverage(sc, m));
    return 0;
}

//::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
// composition group
//::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::

int composition_disk_roundtrip()
{
    Trimesh<> m = flat_disk(8, 8);
    SurfaceCorrespondence sc(m);
    TASK_CHECK(run_collapses(sc, 8) == 8);
    double worst = 0.0;
    TASK_CHECK(roundtrip_from_source(sc, worst));
    TASK_CHECK(worst < EPS);
    return 0;
}

int composition_sphere_roundtrip()
{
    Trimesh<> m = welded_sphere(2);
    SurfaceCorrespondence sc(m);
    TASK_CHECK(run_collapses(sc, 12, 0.5) >= 8);
    double worst = 0.0;
    TASK_CHECK(roundtrip_from_source(sc, worst));
    TASK_CHECK(worst < EPS);
    return 0;
}

int composition_edge_consistency()
{
    Trimesh<> m = welded_sphere(2);
    SurfaceCorrespondence sc(m);
    TASK_CHECK(run_collapses(sc, 10, 0.5) >= 6);
    int checked = 0;
    for(uint e=0; e<m.num_edges(); ++e)
    {
        if(m.edge_valence(e) != 2) continue;
        uint a = m.edge_vert_id(e,0), b = m.edge_vert_id(e,1);
        for(double t : {0.0, 0.25, 0.5, 0.75, 1.0})
        {
            std::vector<SurfaceQuery> qs;
            std::vector<vec3d> pts;
            for(uint pid : m.adj_e2p(e))
            {
                auto vl = m.poly_verts_id(pid);
                std::array<double,3> w{0,0,0};
                for(int k=0;k<3;++k)
                {
                    if(vl[k]==a) w[k] += 1.0-t;
                    if(vl[k]==b) w[k] += t;
                }
                qs.push_back(sc.toCurrent(pid, w));
                pts.push_back(sc.source_point(pid, w));
            }
            // same surface point regardless of the starting face
            TASK_CHECK((pts[0]-pts[1]).norm() < EPS);
            // smallest target face id rule
            TASK_CHECK(qs[0].face == qs[1].face);
            const Trimesh<> & wm = sc.mesh();
            vec3d x0 = wm.poly_sample_at(qs[0].face, qs[0].bary.data());
            vec3d x1 = wm.poly_sample_at(qs[1].face, qs[1].bary.data());
            TASK_CHECK((x0-x1).norm() < EPS);
            ++checked;
        }
        if(checked > 60) break;
    }
    TASK_CHECK(checked > 20);
    return 0;
}

int composition_inverse_query()
{
    Trimesh<> m = bent_patch(7);
    SurfaceCorrespondence sc(m);
    TASK_CHECK(run_collapses(sc, 5) >= 4);
    std::mt19937 rng(424242u);
    std::uniform_real_distribution<double> U(0.0,1.0);
    double worst = 0.0;
    for(uint cf=0; cf<sc.mesh().num_polys(); ++cf)
    {
        double a=U(rng), b=U(rng);
        if(a+b>1.0){ a=1-a; b=1-b; }
        std::array<double,3> w{a,b,1-a-b};
        vec3d p0 = sc.mesh().poly_sample_at(cf, w.data());
        SurfaceQuery s = sc.toSource(cf, w);
        SurfaceQuery q = sc.toCurrent(s.face, s.bary);
        vec3d p1 = sc.mesh().poly_sample_at(q.face, q.bary.data());
        worst = std::max(worst, (p0-p1).norm());
    }
    TASK_CHECK(worst < EPS);
    return 0;
}

//::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
// compaction group
//::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::

int compaction_rewrite_report()
{
    Trimesh<> m = flat_disk(8, 8);
    SurfaceCorrespondence sc(m);
    bool saw_swap = false, saw_reborn = false;
    for(int step=0; step<6; ++step)
    {
        uint before_f = sc.num_current_faces();
        for(uint e=0; e<sc.mesh().num_edges(); ++e)
        {
            if(sc.mesh().edge_is_incident_to_boundary(e)) continue;
            if(sc.collapse(e, 0.4)) break;
        }
        const CollapseRewriteReport & rep = sc.last_report();
        TASK_CHECK(rep.removedFaces.size() == 2);
        TASK_CHECK(rep.keptVert != rep.removedVert);
        // removed and reborn face ids are disjoint
        for(uint rf : rep.removedFaces)
            for(auto pr : rep.rebornFaces) TASK_CHECK(pr.first != rf);
        for(auto pr : rep.rebornFaces)
        {
            TASK_CHECK(pr.second < sc.mesh().num_polys());
            saw_reborn = true;
        }
        if(!rep.faceIdSwaps.empty())
        {
            saw_swap = true;
            // swaps are dense-slot rewrites: both ids reference slots that
            // existed during the operation (including freshly added faces)
            for(auto pr : rep.faceIdSwaps)
            {
                TASK_CHECK(pr.first != pr.second);
                TASK_CHECK(pr.first  < before_f + uint(rep.rebornFaces.size()) + 4);
                TASK_CHECK(pr.second < before_f + uint(rep.rebornFaces.size()) + 4);
            }
        }
    }
    TASK_CHECK(saw_swap);
    TASK_CHECK(saw_reborn);
    TASK_CHECK(check_coverage(sc, m));
    return 0;
}

int compaction_holed_patch()
{
    Trimesh<> m = holed_patch();
    SurfaceCorrespondence sc(m);
    uint start = sc.num_current_faces();
    int done = run_collapses(sc, 8, 0.5);
    TASK_CHECK(done >= 3);
    TASK_CHECK(sc.num_current_faces() == start - 2*done);
    TASK_CHECK(check_coverage(sc, m));
    // boundary (outer and hole) still present: boundary edges remain
    uint bnd = 0;
    for(uint e=0;e<sc.mesh().num_edges();++e)
        if(sc.mesh().edge_is_boundary(e)) ++bnd;
    TASK_CHECK(bnd >= 48);
    return 0;
}

int compaction_two_shells()
{
    Trimesh<> m = two_close_shells();
    SurfaceCorrespondence sc(m);
    uint start = sc.num_current_faces();
    int done = run_collapses(sc, 6, 0.5);
    TASK_CHECK(done >= 3);
    TASK_CHECK(sc.num_current_faces() == start - 2*done);
    TASK_CHECK(check_coverage(sc, m));

    // the two overlapping shells must never be stitched together: a point on
    // the upper shell must map to a current face living on the upper z band
    std::mt19937 rng(11u);
    std::uniform_real_distribution<double> U(0.0,1.0);
    int cross = 0, samples = 0;
    for(uint sf=32; sf<sc.num_source_faces(); ++sf) // second shell source ids
    {
        double a=U(rng), b=U(rng);
        if(a+b>1){a=1-a;b=1-b;}
        std::array<double,3> w{a,b,1-a-b};
        SurfaceQuery q = sc.toCurrent(sf, w);
        auto vv = sc.mesh().poly_verts_id(q.face);
        double zmin = 1e9, zmax = -1e9;
        for(uint vid : vv)
        {
            double z = sc.mesh().vert(vid).z();
            zmin = std::min(zmin, z); zmax = std::max(zmax, z);
        }
        if(zmin < 0.01) ++cross;
        ++samples;
    }
    TASK_CHECK(samples > 0);
    TASK_CHECK(cross == 0);
    return 0;
}

int compaction_successive_local()
{
    Trimesh<> m = flat_disk(8, 8);
    SurfaceCorrespondence sc(m);
    size_t patches_before = sc.patches().size();
    TASK_CHECK(run_collapses(sc, 5) == 5);
    // collapses really refine the map: the patch set grows and stays a cover
    TASK_CHECK(sc.patches().size() > patches_before);
    TASK_CHECK(check_coverage(sc, m));
    return 0;
}

//::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
// rejection group
//::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::

int rejection_bad_arguments()
{
    Trimesh<> m = flat_disk(5, 5);
    SurfaceCorrespondence sc(m);
    bool threw = false;
    try { sc.collapse(sc.mesh().num_edges(), 0.5); }
    catch(const std::invalid_argument &){ threw = true; }
    TASK_CHECK(threw);

    for(double bad : {std::nan(""), -0.1, 1.1})
    {
        threw = false;
        try { sc.collapse(0, bad); }
        catch(const std::invalid_argument &){ threw = true; }
        TASK_CHECK(threw);
    }
    threw = false;
    try { sc.toCurrent(sc.num_source_faces(), {1,0,0}); }
    catch(const std::invalid_argument &){ threw = true; }
    TASK_CHECK(threw);
    threw = false;
    try { sc.toSource(sc.mesh().num_polys(), {1,0,0}); }
    catch(const std::invalid_argument &){ threw = true; }
    TASK_CHECK(threw);
    for(auto bad : std::vector<std::array<double,3>>{{0.5,0.6,0.6},{-0.1,0.5,0.6},{0,0,0}})
    {
        threw = false;
        try { sc.toCurrent(0, bad); }
        catch(const std::invalid_argument &){ threw = true; }
        TASK_CHECK(threw);
    }
    return 0;
}

int rejection_boundary_and_atomicity()
{
    Trimesh<> m = bent_patch(6);
    SurfaceCorrespondence sc(m);
    TASK_CHECK(run_collapses(sc, 2) == 2);

    // every boundary incident edge is rejected, never throws
    for(uint e=0;e<sc.mesh().num_edges();++e)
    {
        if(!sc.mesh().edge_is_incident_to_boundary(e)) continue;
        TASK_CHECK(sc.collapse(e, 0.5) == false);
    }

    // a failed candidate leaves mesh and patches exactly as they were
    uint faces = sc.num_current_faces();
    size_t np = sc.patches().size();
    bool any_failure = false;
    for(uint e=0;e<sc.mesh().num_edges();++e)
    {
        if(sc.mesh().edge_is_incident_to_boundary(e)) continue;
        if(!sc.collapse(e, 0.0)) // lambda 0 often creates degenerate geometry -> reject
        {
            any_failure = true;
            break;
        }
    }
    TASK_CHECK(any_failure);
    TASK_CHECK(sc.num_current_faces() == faces);
    TASK_CHECK(sc.patches().size() == np);
    TASK_CHECK(check_coverage(sc, m));
    return 0;
}

int rejection_source_independence()
{
    Trimesh<> m = flat_disk(5, 5);
    SurfaceCorrespondence sc(m);
    run_collapses(sc, 3);
    uint faces = sc.num_current_faces();
    size_t np = sc.patches().size();
    // mutate the source mesh externally after the session started
    m.vert(0) = vec3d(123.0, 456.0, 789.0);
    TASK_CHECK(sc.num_current_faces() == faces);
    TASK_CHECK(sc.patches().size() == np);
    // the immutable source points are untouched by the external mutation
    vec3d p = sc.source_point(0, {1,0,0});
    TASK_CHECK(std::fabs(p.x() - 0.0) < EPS);
    return 0;
}

int rejection_flat_refinement_reduces()
{
    // a regular flat refined grid must admit real reductions
    Trimesh<> m = flat_disk(7, 7);
    SurfaceCorrespondence sc(m);
    uint start_f = sc.num_current_faces();
    int done = run_collapses(sc, 5);
    TASK_CHECK(done == 5);
    TASK_CHECK(sc.num_current_faces() == start_f - 10);
    double worst = 0.0;
    TASK_CHECK(roundtrip_from_source(sc, worst));
    TASK_CHECK(worst < EPS);
    return 0;
}

} // anonymous namespace

//::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
// registration
//::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::

const struct task_case task_cases[] =
{
    {"maps",        "identity_patches",        maps_identity},
    {"maps",        "disk_coverage",           maps_disk_coverage},
    {"maps",        "sphere_orientation",      maps_sphere_orientation},
    {"maps",        "bent_coverage",           maps_bent_coverage},

    {"composition", "disk_roundtrip",          composition_disk_roundtrip},
    {"composition", "sphere_roundtrip",        composition_sphere_roundtrip},
    {"composition", "edge_consistency",        composition_edge_consistency},
    {"composition", "inverse_query",           composition_inverse_query},

    {"compaction",  "rewrite_report",          compaction_rewrite_report},
    {"compaction",  "holed_patch",             compaction_holed_patch},
    {"compaction",  "two_close_shells",        compaction_two_shells},
    {"compaction",  "successive_local",        compaction_successive_local},

    {"rejection",   "bad_arguments",           rejection_bad_arguments},
    {"rejection",   "boundary_atomicity",      rejection_boundary_and_atomicity},
    {"rejection",   "source_independence",     rejection_source_independence},
    {"rejection",   "flat_refinement_reduces", rejection_flat_refinement_reduces},
};

const size_t task_case_count = sizeof(task_cases)/sizeof(task_cases[0]);
