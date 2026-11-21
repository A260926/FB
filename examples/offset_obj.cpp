// Example: fit a non-planar normal offset of a rational quarter-cylinder
// NURBS surface, persist the fit through the library's own
// SaveToFile/LoadFromFile round-trip, and tessellate the reloaded surface
// into a Wavefront OBJ file (v / vn / f) using result evaluation together
// with the analytic surface normal.
//
// Usage: offset-example <output_obj_path>
// Returns a non-zero process status when fitting, serialization or file
// writing fails, so callers never silently skip a failed example.

#include <cmath>
#include <cstdio>
#include <fstream>
#include <string>

#include <libnurbs/Surface/Surface.hpp>
#include <libnurbs/Surface/OffsetFit.hpp>

using namespace libnurbs;

namespace
{
    Surface MakeQuarterCylinder()
    {
        Surface surface;
        surface.DegreeU = surface.DegreeV = 2;
        surface.KnotsU = surface.KnotsV = KnotVector{{0, 0, 0, 1, 1, 1}};
        surface.ControlPoints = ControlPointGrid(3, 3);
        const double wq = std::sqrt(2.0) / 2.0;
        const Vec3 ring[3] = {Vec3(1, 0, 0), Vec3(1, 0, 1), Vec3(0, 0, 1)};
        const double weight[3] = {1.0, wq, 1.0};
        for (int j = 0; j < 3; ++j)
            for (int i = 0; i < 3; ++i)
                surface.ControlPoints.Get(i, j) =
                    Vec4(ring[i].x(), j / 2.0, ring[i].z(), weight[i]);
        return surface;
    }
}

int main(int argc, char** argv)
{
    if (argc < 2 || argv[1] == nullptr || argv[1][0] == '\0')
    {
        std::fprintf(stderr, "usage: %s <output_obj_path>\n",
                     argc > 0 ? argv[0] : "offset-example");
        return 2;
    }
    const std::string obj_path = argv[1];

    // 1. Fit a genuinely non-planar normal offset (positive distance).
    const Surface source = MakeQuarterCylinder();
    OffsetOptions options;
    options.distance = 0.12;
    options.tolerance = 1e-3;
    options.max_control_points = 256;
    options.max_boxes = 4096;
    const OffsetFitResult fit = FitNormalOffset(source, options);
    if (fit.Status != OffsetFitStatus::success)
    {
        std::fprintf(stderr, "offset fit failed with status %d\n",
                     static_cast<int>(fit.Status));
        return 3;
    }
    if (fit.Fit.ControlPoints.Values.empty())
    {
        std::fprintf(stderr, "fit published no surface\n");
        return 3;
    }

    // 2. Original text and binary SaveToFile/LoadFromFile round-trip. The
    //    text archive is what the tessellation below reads back.
    const std::string archive_path = obj_path + ".nurbs";
    const std::string binary_path = obj_path + ".nurbs.bin";
    Surface reloaded;
    try
    {
        fit.Fit.SaveToFile(archive_path, false);
        reloaded.LoadFromFile(archive_path);
        fit.Fit.SaveToFile(binary_path, true);
        Surface reloaded_bin;
        reloaded_bin.LoadFromFile(binary_path);
        double roundtrip_error = 0.0;
        for (int j = 0; j <= 8; ++j)
            for (int i = 0; i <= 8; ++i)
            {
                const double u = i / 8.0;
                const double v = j / 8.0;
                roundtrip_error = std::max(
                    roundtrip_error,
                    (reloaded_bin.Evaluate(u, v) - fit.Fit.Evaluate(u, v)).norm());
            }
        if (!std::isfinite(roundtrip_error) || roundtrip_error > 1e-9)
        {
            std::fprintf(stderr, "serialization round-trip error %g\n", roundtrip_error);
            return 4;
        }
    }
    catch (const std::exception& exception)
    {
        std::fprintf(stderr, "serialization failed: %s\n", exception.what());
        return 4;
    }

    // 3. Tessellate the reloaded fitted surface and write OBJ.
    std::ofstream obj(obj_path);
    if (!obj.is_open())
    {
        std::fprintf(stderr, "cannot open output OBJ: %s\n", obj_path.c_str());
        return 5;
    }
    obj << "# libnurbs normal-offset quarter cylinder, d=" << options.distance << "\n";

    constexpr int kSegments = 24;
    long vertex_count = 0;
    long normal_count = 0;
    long face_count = 0;
    auto index = [](int i, int j) { return j * (kSegments + 1) + i + 1; };

    for (int j = 0; j <= kSegments; ++j)
    {
        const double v = static_cast<double>(j) / kSegments;
        for (int i = 0; i <= kSegments; ++i)
        {
            const double u = static_cast<double>(i) / kSegments;
            const Vec3 position = reloaded.Evaluate(u, v);
            // Analytic unit normal of the offset layer: S_u x S_v /
            // |S_u x S_v| of the source, evaluated independently of the fit.
            const Grid<Vec3> jet = source.EvaluateAll(u, v, 1, 1);
            const Vec3 normal =
                jet.Get(1, 0).cross(jet.Get(0, 1)).normalized();

            obj << "v " << position.x() << ' ' << position.y() << ' ' << position.z() << '\n';
            obj << "vn " << normal.x() << ' ' << normal.y() << ' ' << normal.z() << '\n';
            ++vertex_count;
            ++normal_count;
        }
    }

    for (int j = 0; j < kSegments; ++j)
    {
        for (int i = 0; i < kSegments; ++i)
        {
            const int a = index(i, j);
            const int b = index(i + 1, j);
            const int c = index(i + 1, j + 1);
            const int d = index(i, j + 1);
            obj << "f " << a << "//" << a << ' ' << b << "//" << b << ' '
                << c << "//" << c << '\n';
            obj << "f " << a << "//" << a << ' ' << c << "//" << c << ' '
                << d << "//" << d << '\n';
            face_count += 2;
        }
    }
    obj.close();
    if (obj.fail())
    {
        std::fprintf(stderr, "failed while writing OBJ\n");
        return 6;
    }

    if (vertex_count == 0 || normal_count == 0 || face_count == 0)
    {
        std::fprintf(stderr, "produced empty OBJ\n");
        return 7;
    }
    std::printf("offset-example wrote %ld vertices, %ld normals, %ld faces to %s\n",
                vertex_count, normal_count, face_count, obj_path.c_str());
    return 0;
}
