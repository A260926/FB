// Owned Catmull-Clark subdivision plan example.
//
// Builds a small control cage with a UV seam, compiles a SubdivisionPlan,
// demonstrates that the plan can be evaluated repeatedly with new control
// positions and new channel values, and writes:
//
//   before.obj  control cage with the first set of control values
//   after.obj   two-level subdivision of the second set of control values
//
// Both files carry independent per-corner vt indices.
//
// Usage: subdivision_plan <output-directory>

#include "pmp/algorithms/subdivision_plan.h"
#include "pmp/surface_mesh.h"

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace pmp;

namespace {

// A 2x2 quad grid in the x-y plane with an interior UV seam.
struct Cage
{
    SurfaceMesh mesh;
    std::vector<Vertex> vertices;
    std::vector<SubdivisionPlanChannel> channels;
};

Cage make_cage()
{
    Cage cage;
    const int n = 2;
    cage.vertices.resize(size_t((n + 1) * (n + 1)));
    for (int i = 0; i <= n; ++i)
        for (int j = 0; j <= n; ++j)
        {
            const Scalar x = Scalar(i) / n;
            const Scalar y = Scalar(j) / n;
            cage.vertices[size_t(i * (n + 1) + j)] =
                cage.mesh.add_vertex(Point(x, y, Scalar(0.15) * x * y));
        }
    for (int i = 0; i < n; ++i)
        for (int j = 0; j < n; ++j)
            cage.mesh.add_quad(
                cage.vertices[size_t(i * (n + 1) + j)],
                cage.vertices[size_t((i + 1) * (n + 1) + j)],
                cage.vertices[size_t((i + 1) * (n + 1) + j + 1)],
                cage.vertices[size_t(i * (n + 1) + j + 1)]);

    // Channel "uv": a seam down the middle column. The right half uses a
    // duplicate set of value indices with identical numeric values, so the
    // seam is an index seam rather than a geometric one.
    const size_t stride = cage.mesh.vertices_size();
    SubdivisionPlanChannel uv;
    uv.name = "uv";
    uv.values.resize(2 * stride, TexCoord(0, 0));
    for (auto v : cage.mesh.vertices())
        if (!cage.mesh.is_deleted(v))
        {
            const TexCoord t(cage.mesh.position(v)[0],
                             cage.mesh.position(v)[1]);
            uv.values[v.idx()] = t;
            uv.values[v.idx() + stride] = t;
        }
    uv.corner_values.assign(cage.mesh.halfedges_size(), 0);
    for (auto h : cage.mesh.halfedges())
    {
        if (cage.mesh.is_deleted(h))
            continue;
        const Vertex v = cage.mesh.to_vertex(h);
        const int i = int(v.idx()) / (n + 1);
        const int face_i = int(cage.mesh.face(h).idx()) / n;
        const bool right = (i == 1 && face_i == 1) || (i == 2 && face_i == 1);
        uv.corner_values[h.idx()] =
            IndexType(v.idx() + (right ? stride : 0));
    }
    cage.channels.push_back(std::move(uv));
    return cage;
}

// Write positions, channel-0 values and faces as Wavefront OBJ, with
// independent vt indices per corner.
void write_obj(const std::filesystem::path& path,
               const std::vector<Point>& positions,
               const std::vector<TexCoord>& values,
               const std::vector<std::vector<IndexType>>& faces,
               const std::vector<std::vector<IndexType>>& corners)
{
    std::ofstream out(path);
    if (!out)
        throw std::runtime_error("cannot open " + path.string());
    out << "# OBJ export from SubdivisionPlan example\n";
    for (const Point& p : positions)
        out << "v " << p[0] << ' ' << p[1] << ' ' << p[2] << '\n';
    for (const TexCoord& t : values)
        out << "vt " << t[0] << ' ' << t[1] << '\n';
    for (size_t f = 0; f < faces.size(); ++f)
    {
        out << 'f';
        for (size_t k = 0; k < faces[f].size(); ++k)
        {
            const IndexType v = faces[f][k] + 1;
            const IndexType vt = corners[f][k] + 1;
            out << ' ' << v << '/' << vt;
        }
        out << '\n';
    }
}

std::vector<Point> control_positions(const Cage& cage,
                                     const SubdivisionPlan& plan)
{
    std::vector<Point> positions(plan.source_vertex_count());
    for (size_t i = 0; i < positions.size(); ++i)
        positions[i] = cage.mesh.position(plan.source_vertex(i));
    return positions;
}

// Source faces and per-corner value indices in the plan's compact order.
void source_face_layout(const Cage& cage, const SubdivisionPlan& plan,
                        std::vector<std::vector<IndexType>>& faces,
                        std::vector<std::vector<IndexType>>& corners)
{
    faces.clear();
    corners.clear();
    const auto& value_indices = plan.channel_source_value_indices(0);
    for (auto f : cage.mesh.faces())
    {
        if (cage.mesh.is_deleted(f))
            continue;
        std::vector<IndexType> face;
        std::vector<IndexType> corner;
        for (auto h : cage.mesh.halfedges(f))
        {
            face.push_back(
                plan.source_vertex_id(cage.mesh.to_vertex(h)));
            const IndexType cid = plan.source_corner_id(h);
            corner.push_back(value_indices[cid]);
        }
        faces.push_back(std::move(face));
        corners.push_back(std::move(corner));
    }
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        std::fprintf(stderr, "usage: %s <output-directory>\n", argv[0]);
        return 1;
    }
    const std::filesystem::path dir = argv[1];
    std::filesystem::create_directories(dir);

    Cage cage = make_cage();

    // Compile the owned plan once.
    SubdivisionPlan plan(cage.mesh, 2, cage.channels);

    const std::vector<Point> base_positions = control_positions(cage, plan);
    const std::vector<TexCoord> base_values = cage.channels[0].values;

    // First control value set: the cage as built.
    plan.evaluate(base_positions, {base_values});

    std::vector<std::vector<IndexType>> faces;
    std::vector<std::vector<IndexType>> corners;
    source_face_layout(cage, plan, faces, corners);
    write_obj(dir / "before.obj", base_positions, base_values, faces, corners);

    // Second control value set: move the control vertices and recolour the
    // channel. The plan is not rebuilt; only the stored stencils are applied.
    std::vector<Point> moved = base_positions;
    for (size_t i = 0; i < moved.size(); ++i)
    {
        const Scalar s = Scalar(i);
        moved[i] = moved[i] + Point(Scalar(0.1) * std::sin(s),
                                    Scalar(0.1) * std::cos(s),
                                    Scalar(0.2));
    }
    std::vector<TexCoord> recoloured = base_values;
    for (size_t i = 0; i < recoloured.size(); ++i)
        recoloured[i] = TexCoord(Scalar(1) - recoloured[i][0],
                                 recoloured[i][1] * 0.5 + 0.25);

    plan.evaluate(moved, {recoloured});

    std::ofstream info(dir / "subdivision_plan.txt");
    if (info)
    {
        info << "levels " << plan.levels() << '\n'
             << "source_vertices " << plan.source_vertex_count() << '\n'
             << "source_faces " << plan.source_face_count() << '\n'
             << "source_corners " << plan.source_corner_count() << '\n'
             << "result_vertices " << plan.vertex_count() << '\n'
             << "result_faces " << plan.face_count() << '\n'
             << "channel_value_count " << plan.channel_value_count(0) << '\n'
             << "channel_control_vertices "
             << plan.channel_control_vertex_count(0) << '\n';
    }

    write_obj(dir / "after.obj", plan.positions(), plan.channel_values(0),
              plan.faces(), plan.channel_corner_indices(0));

    std::printf("wrote %s and %s (result %zu vertices, %zu faces)\n",
                (dir / "before.obj").string().c_str(),
                (dir / "after.obj").string().c_str(), plan.vertex_count(),
                plan.face_count());
    return 0;
}
