// Example: build an owning Catmull-Clark subdivision plan once and evaluate
// it for two different sets of control values. Writes before.obj and
// after.obj with independent vt indices to the directory given in argv[1].

#include "pmp/algorithms/subdivision_plan.h"
#include "pmp/algorithms/shapes.h"

#include <filesystem>
#include <fstream>
#include <iostream>

using namespace pmp;

namespace {

void write_obj(const std::filesystem::path& path, const SubdivisionPlan& plan)
{
    std::ofstream os(path);
    os << "# Catmull-Clark subdivision plan output (level " << plan.levels()
       << ")\n";
    for (const auto& p : plan.positions())
        os << "v " << p[0] << ' ' << p[1] << ' ' << p[2] << '\n';
    for (const auto& t : plan.channel_values(0))
        os << "vt " << t[0] << ' ' << t[1] << '\n';
    // shared geometric connectivity with independent per-corner vt indices
    for (size_t f = 0; f < plan.n_faces(); ++f)
    {
        os << 'f';
        for (size_t j = 0; j < 4; ++j)
            os << ' ' << plan.faces()[4 * f + j] + 1 << '/'
               << plan.channel_corners(0)[4 * f + j] + 1;
        os << '\n';
    }
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        std::cerr << "usage: subdivision_plan <output_dir>\n";
        return 1;
    }
    const std::filesystem::path outdir(argv[1]);
    std::error_code ec;
    std::filesystem::create_directories(outdir, ec);
    if (ec)
    {
        std::cerr << "cannot create output directory: " << ec.message()
                  << '\n';
        return 1;
    }

    try
    {
        // control cage: slightly sheared cube
        SurfaceMesh mesh = hexahedron();
        for (auto v : mesh.vertices())
        {
            const auto q = mesh.position(v);
            mesh.position(v) =
                Point(q[0] + 0.2 * q[1], 0.9 * q[1], q[2] - 0.15 * q[0]);
        }

        // one texture channel: each cube face owns its four corners and maps
        // them to the unit square, so every edge is a UV seam
        SubdivisionPlan::Channel uv;
        for (auto f : mesh.faces())
        {
            size_t j = 0;
            for (auto h : mesh.halfedges(f))
            {
                (void)h;
                uv.corner_indices.push_back(IndexType(uv.values.size()));
                uv.values.emplace_back(j == 1 || j == 2 ? 1.0 : 0.0,
                                       j >= 2 ? 1.0 : 0.0);
                ++j;
            }
        }

        SubdivisionPlan plan;
        plan.build(mesh, 3, {uv});

        // first control set: the cage as built
        std::vector<Point> positions(plan.n_source_vertices());
        for (size_t i = 0; i < positions.size(); ++i)
            positions[i] = mesh.position(plan.source_vertex(i));
        plan.evaluate(positions, {uv.values});
        write_obj(outdir / "before.obj", plan);
        std::cout << "wrote before.obj: " << plan.n_vertices()
                  << " vertices, " << plan.n_faces() << " quads, "
                  << plan.n_channel_vertices(0) << " texcoords\n";

        // second control set: twist the cage and shift the texture
        for (size_t i = 0; i < positions.size(); ++i)
        {
            const auto& q = positions[i];
            positions[i] = Point(q[0] - 0.3 * q[2], q[1], q[2] + 0.3 * q[0]);
        }
        std::vector<TexCoord> shifted = uv.values;
        for (auto& t : shifted)
            t = TexCoord(0.5 * t[0] + 0.25, 0.5 * t[1] + 0.25);
        plan.evaluate(positions, {shifted});
        write_obj(outdir / "after.obj", plan);
        std::cout << "wrote after.obj with the same plan\n";
    }
    catch (const std::exception& e)
    {
        std::cerr << "subdivision_plan example failed: " << e.what() << '\n';
        return 1;
    }
    return 0;
}
