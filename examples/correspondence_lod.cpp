// Correspondence LOD example: build a curved open triangle patch, coarsen it
// through a SurfaceCorrespondence session and emit three LOD meshes plus a
// CSV that follows a fixed set of source-side painted points through every
// level of detail.
//
// usage: correspondence_lod <output directory>

#include <cinolib/surface_correspondence.h>
#include <cinolib/io/write_OBJ.h>

#include <cmath>
#include <fstream>
#include <random>
#include <string>
#include <vector>

using namespace cinolib;

namespace
{

// curved open patch: an asymmetric, gently bent triangle grid with boundary
Trimesh<> make_patch(unsigned nx, unsigned ny)
{
    std::vector<vec3d> v;
    std::vector<uint> f;
    for(unsigned y=0;y<ny;++y)
        for(unsigned x=0;x<nx;++x)
        {
            double z = 0.25*std::sin(0.7*double(x))
                     + 0.18*std::cos(0.9*double(y))
                     + 0.02*double(x);
            v.emplace_back(double(x), double(y), z);
        }
    for(unsigned y=0;y+1<ny;++y)
        for(unsigned x=0;x+1<nx;++x)
        {
            uint a = x + nx*y;
            f.insert(f.end(), {a, a+1, a+nx+1, a, a+nx+1, a+nx});
        }
    return Trimesh<>(v, f);
}

void write_mesh_obj(const std::string & path, const Trimesh<> & m)
{
    std::vector<double> xyz;
    for(const vec3d & p : m.vector_verts())
    {
        xyz.push_back(p.x());
        xyz.push_back(p.y());
        xyz.push_back(p.z());
    }
    std::vector<uint> tri, quad;
    for(const auto & p : m.vector_polys())
    {
        if(p.size() == 3) tri.insert(tri.end(), p.begin(), p.end());
        else              for(uint vid : p) quad.push_back(vid);
    }
    write_OBJ(path.c_str(), xyz, tri, quad);
}

struct Painted { uint face; std::array<double,3> bary; };
struct LodColumn { SurfaceQuery query; vec3d point; };

} // anonymous namespace

int main(int argc, char ** argv)
{
    std::string outdir = (argc > 1) ? std::string(argv[1]) : std::string(".");
    if(!outdir.empty() && outdir.back() != '/') outdir.push_back('/');

    Trimesh<> source = make_patch(9, 8);
    SurfaceCorrespondence sc(source);

    // fixed deterministic set of source-side painted points: interior samples,
    // plus edge/vertex points so the CSV exercises boundary handling
    std::vector<Painted> painted;
    std::mt19937 rng(20240101u);
    std::uniform_real_distribution<double> U(0.05, 0.95);
    for(uint f=0; f<sc.num_source_faces(); ++f)
    {
        double a=U(rng), b=U(rng);
        if(a+b > 0.95) { a *= 0.5; b *= 0.5; }
        painted.push_back(Painted{f, {a, b, 1.0-a-b}});
    }
    for(uint e=0; e<source.num_edges() && painted.size() < 90; ++e)
    {
        if(source.edge_valence(e) != 2) continue;
        uint a=source.edge_vert_id(e,0), b=source.edge_vert_id(e,1);
        for(uint pid : source.adj_e2p(e))
        {
            auto vl = source.poly_verts_id(pid);
            std::array<double,3> w{0,0,0};
            for(int k=0;k<3;++k)
            {
                if(vl[k]==a) w[k] += 0.5;
                if(vl[k]==b) w[k] += 0.5;
            }
            painted.push_back(Painted{pid, w});
            break;
        }
    }

    auto run = [&](int target)
    {
        int done = 0;
        for(int s=0; s<target; ++s)
        {
            bool ok = false;
            for(uint e=0; e<sc.mesh().num_edges(); ++e)
            {
                if(sc.mesh().edge_is_incident_to_boundary(e)) continue;
                if(sc.collapse(e, 0.4)) { ok = true; break; }
            }
            if(!ok) break;
            ++done;
        }
        return done;
    };

    // LOD 0: identity
    std::vector<LodColumn> col0(painted.size());
    for(size_t i=0;i<painted.size();++i)
    {
        col0[i].query.face = painted[i].face;
        col0[i].query.bary = painted[i].bary;
        col0[i].point = sc.source_point(painted[i].face, painted[i].bary);
    }
    write_mesh_obj(outdir + "original.obj", source);

    // LOD 1: first batch of collapses
    int d1 = run(6);
    std::vector<LodColumn> col1(painted.size());
    for(size_t i=0;i<painted.size();++i)
    {
        col1[i].query = sc.toCurrent(painted[i].face, painted[i].bary);
        col1[i].point = sc.mesh().poly_sample_at(col1[i].query.face, col1[i].query.bary.data());
    }
    write_mesh_obj(outdir + "level1.obj", sc.mesh());

    // LOD 2: second batch (the same painted points, still tied to the immutable source)
    int d2 = run(12);
    std::vector<LodColumn> col2(painted.size());
    for(size_t i=0;i<painted.size();++i)
    {
        col2[i].query = sc.toCurrent(painted[i].face, painted[i].bary);
        col2[i].point = sc.mesh().poly_sample_at(col2[i].query.face, col2[i].query.bary.data());
    }
    write_mesh_obj(outdir + "level2.obj", sc.mesh());

    std::ofstream out(outdir + "points.csv");
    out << "sourceFace,srcBary0,srcBary1,srcBary2,"
           "lod0Face,lod0Bary0,lod0Bary1,lod0Bary2,lod0X,lod0Y,lod0Z,"
           "lod1Face,lod1Bary0,lod1Bary1,lod1Bary2,lod1X,lod1Y,lod1Z,"
           "lod2Face,lod2Bary0,lod2Bary1,lod2Bary2,lod2X,lod2Y,lod2Z\n";
    auto emit = [&](const LodColumn & c){
        out << "," << c.query.face
            << "," << c.query.bary[0] << "," << c.query.bary[1] << "," << c.query.bary[2]
            << "," << c.point.x() << "," << c.point.y() << "," << c.point.z();
    };
    for(size_t i=0;i<painted.size();++i)
    {
        const Painted & pt = painted[i];
        out << pt.face << "," << pt.bary[0] << "," << pt.bary[1] << "," << pt.bary[2];
        emit(col0[i]);
        emit(col1[i]);
        emit(col2[i]);
        out << "\n";
    }
    out.close();

    std::printf("source faces=%u  level1 faces=%u (%d collapses)  level2 faces=%u (%d collapses)\n",
                sc.num_source_faces(),
                sc.num_source_faces() - 2u*uint(d1), d1,
                sc.num_current_faces(), d1 + d2);
    return 0;
}
