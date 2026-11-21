// Example: fit a flat editable triangle patch onto a non-planar scanned
// target surface using HMesh::SurfaceFitSession.
//
// Usage: surface_fit <output.obj>
// Writes the fitted source mesh as an OBJ file to <output.obj> and exits
// non-zero if the file cannot be written.

#include <cmath>
#include <cstdio>
#include <fstream>
#include <string>
#include <unordered_map>
#include <vector>

#include <GEL/HMesh/HMesh.h>
#include <GEL/HMesh/SurfaceFit.h>

using namespace CGLA;
using namespace HMesh;

namespace
{
    // A deliberately non-planar target patch: a low frequency wave.
    Vec3d target_point(double x, double y)
    {
        return Vec3d(x, y, 0.45*std::sin(2.2*x)*std::cos(1.7*y) + 0.15*x*y);
    }

    void build_grid(Manifold& m, int n, bool flat_source)
    {
        std::vector<Vec3d> points;
        std::vector<size_t> faces;
        points.reserve(static_cast<size_t>(n*n));
        for(int j=0;j<n;++j)
            for(int i=0;i<n;++i)
            {
                const double x = -1.0 + 2.0*i/(n-1);
                const double y = -1.0 + 2.0*j/(n-1);
                if(flat_source)
                    points.emplace_back(x, y, -0.55);
                else
                    points.push_back(target_point(x, y));
            }
        auto idx = [n](int i, int j) { return static_cast<size_t>(j*n+i); };
        for(int j=0;j+1<n;++j)
            for(int i=0;i+1<n;++i)
            {
                const size_t a = idx(i,j), b = idx(i+1,j),
                             c = idx(i+1,j+1), e = idx(i,j+1);
                faces.insert(faces.end(), {a, b, c, a, c, e});
            }
        build_manifold(m, points, faces, 3);
    }
}

int main(int argc, char** argv)
{
    if(argc != 2)
    {
        std::fprintf(stderr,
            "Usage: %s <output.obj>\n"
            "Fits a flat triangle patch onto a non-planar target surface and\n"
            "writes the fitted source mesh (OBJ) to the given path.\n",
            argv[0]);
        return 2;
    }

    constexpr int kGrid = 12;
    Manifold source;
    build_grid(source, kGrid, true);

    Manifold target;
    build_grid(target, kGrid, false);

    // Pin the boundary ring so the patch keeps its footprint.
    SurfaceFitOptions options;
    options.lambda = 0.05; // Laplacian smoothing of the displacement field
    options.mu = 1.0e-3;   // weak pull towards the initial positions
    options.d = 1.5;       // matches farther away than this are truncated
    for(auto v: source.vertices())
        if(source.boundary(v))
            options.fixed_vertices.push_back(v);

    SurfaceFitSession session(source, target, options);

    // The session owns an independent snapshot: modifying or destroying the
    // target after construction must not change any later correspondence.
    target.clear();

    std::printf("Surface fit example: %zu source vertices, %zu surface samples\n",
                session.vertex_count(), session.sample_count());
    std::puts("Energy E = sum_s w_s min(dist(q_s,target)^2, d^2)"
              " + lambda*||L(X-X0)||^2 + mu*||X-X0||^2");
    std::puts("iteration status alpha energy_before energy_after active_samples");

    for(int it=0; it<14; ++it)
    {
        SurfaceFitReport r = session.iterate(120);
        const char* tag =
            r.status == SurfaceFitStatus::progress ? "progress" :
            r.status == SurfaceFitStatus::stationary ? "stationary" : "no_progress";
        std::printf("%d %s %.6g %.9g %.9g %zu\n", it, tag, r.alpha,
                    r.energy_before, r.energy_after, r.active_samples);
        if(r.status != SurfaceFitStatus::progress)
            break;
    }

    // Write the fitted mesh as OBJ ourselves so a path that cannot be
    // opened or written is detected and reported as a non-zero exit.
    std::ofstream os(argv[1]);
    os.precision(17);
    if(!os)
    {
        std::fprintf(stderr, "Failed to open OBJ file: %s\n", argv[1]);
        return 1;
    }
    std::unordered_map<VertexID, int, std::hash<VertexID>> vmap;
    int index = 1;
    for(auto v: source.vertices())
    {
        const Vec3d p = source.pos(v);
        vmap[v] = index++;
        os << "v " << p[0] << " " << p[1] << " " << p[2] << "\n";
    }
    for(auto f: source.faces())
    {
        os << "f";
        for(auto vid: source.incident_vertices(f))
            os << " " << vmap[vid];
        os << "\n";
    }
    os.flush();
    if(!os)
    {
        std::fprintf(stderr, "Failed to write OBJ file: %s\n", argv[1]);
        return 1;
    }
    std::printf("Wrote fitted mesh to %s\n", argv[1]);
    return 0;
}
