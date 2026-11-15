// Domain-preserving mesh export example.
//
// Builds a rational surface on a non-unit parameter domain, edits it through
// the production knot-insertion / degree-elevation / file round-trip paths,
// tessellates it with the new SurfaceMesh export and writes an OBJ file with
// v/vt/vn/f records. The example re-reads its own output and verifies that the
// vertex and face counts are non-zero.

#include <libnurbs/Curve/Curve.hpp>
#include <libnurbs/Surface/Surface.hpp>
#include <libnurbs/Surface/SurfaceMesh.hpp>

#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace libnurbs;

namespace
{
    // Rational surface over U = [-3, 5], V = [10, 12]:
    // a parabola-ish ridge in U and a gentle arc in V.
    Surface MakeRationalPatch()
    {
        const double k = std::sqrt(2.0) / 2.0;
        Surface surface;
        surface.DegreeU = 2;
        surface.DegreeV = 1;
        surface.KnotsU = KnotVector{{-3, -3, -3, 5, 5, 5}};
        surface.KnotsV = KnotVector{{10, 10, 12, 12}};
        surface.ControlPoints = ControlPointGrid(3, 2);
        surface.ControlPoints.Get(0, 0) = Vec4(1, 0, 0, 1);
        surface.ControlPoints.Get(1, 0) = Vec4(1, 1, 0, k);
        surface.ControlPoints.Get(2, 0) = Vec4(0, 1, 0, 1);
        surface.ControlPoints.Get(0, 1) = Vec4(1, 0, 1.5, 1);
        surface.ControlPoints.Get(1, 1) = Vec4(1, 1, 2.2, k);
        surface.ControlPoints.Get(2, 1) = Vec4(0, 1, 1.5, 1);
        return surface;
    }

    Surface RoundTrip(const Surface& surface)
    {
        std::stringstream stream(std::ios::in | std::ios::out | std::ios::binary);
        surface.SaveToFile(stream, true);
        stream.seekg(0);
        Surface loaded;
        loaded.LoadFromFile(stream);
        return loaded;
    }

    bool WriteObj(const std::string& path, const SurfaceMesh& mesh)
    {
        std::ofstream out(path);
        if (!out.is_open()) return false;
        out.precision(17);
        for (const auto& position : mesh.positions)
        {
            out << "v " << position.x() << " " << position.y() << " " << position.z() << "\n";
        }
        for (const auto& uv : mesh.corner_uv)
        {
            out << "vt " << uv.x() << " " << uv.y() << "\n";
        }
        for (const auto& normal : mesh.corner_normals)
        {
            out << "vn " << normal.x() << " " << normal.y() << " " << normal.z() << "\n";
        }
        // OBJ indices are one-based and reference vt/vn per corner.
        for (size_t t = 0; t < mesh.triangles.size(); ++t)
        {
            out << "f";
            for (int corner = 0; corner < 3; ++corner)
            {
                const int vertex = mesh.triangles[t][corner] + 1;
                const int attribute = static_cast<int>(t * 3 + corner) + 1;
                out << " " << vertex << "/" << attribute << "/" << attribute;
            }
            out << "\n";
        }
        return static_cast<bool>(out);
    }
} // namespace

int main(int argc, char** argv)
{
    if (argc != 2)
    {
        std::cerr << "usage: domain_mesh <output.obj>\n";
        return 2;
    }
    const std::string path = argv[1];

    try
    {
        Surface surface = MakeRationalPatch();

        // Keep the original domain while editing through production paths.
        surface = surface.InsertKnotU(-1.0).InsertKnotV(11.0).ElevateDegreeU(1);
        const auto [u_begin, u_end] = surface.GetDomainU();
        const auto [v_begin, v_end] = surface.GetDomainV();
        if (!(std::abs(u_begin + 3.0) < 1e-12 && std::abs(u_end - 5.0) < 1e-12))
        {
            std::cerr << "unexpected U domain after editing\n";
            return 1;
        }
        if (!(std::abs(v_begin - 10.0) < 1e-12 && std::abs(v_end - 12.0) < 1e-12))
        {
            std::cerr << "unexpected V domain after editing\n";
            return 1;
        }

        // File round-trip must not move the geometry or the domain.
        Surface reloaded = RoundTrip(surface);
        const Vec3 before = surface.Evaluate(0.5 * (u_begin + u_end), 0.5 * (v_begin + v_end));
        const Vec3 after = reloaded.Evaluate(0.5 * (u_begin + u_end), 0.5 * (v_begin + v_end));
        if ((before - after).norm() > 1e-12)
        {
            std::cerr << "round-trip changed the geometry\n";
            return 1;
        }

        const std::vector<double> us{u_begin, u_begin + 2.0, u_begin + 4.0, u_begin + 5.5, u_end};
        const std::vector<double> vs{v_begin, v_begin + 0.75, v_begin + 1.4, v_end};
        SurfaceMesh mesh = BuildSurfaceMesh(reloaded, us, vs);

        if (!WriteObj(path, mesh))
        {
            std::cerr << "failed to write " << path << "\n";
            return 1;
        }

        // Validate our own output: non-zero vertices and faces, every face
        // references existing vertices.
        std::ifstream verify(path);
        if (!verify.is_open())
        {
            std::cerr << "failed to reopen " << path << "\n";
            return 1;
        }
        size_t vertex_count = 0;
        size_t face_count = 0;
        size_t max_index = 0;
        std::string line;
        while (std::getline(verify, line))
        {
            if (line.rfind("v ", 0) == 0) ++vertex_count;
            else if (line.rfind("f ", 0) == 0)
            {
                ++face_count;
                std::istringstream ss(line.substr(2));
                std::string token;
                while (ss >> token)
                {
                    const size_t slash = token.find('/');
                    const size_t index = std::stoul(token.substr(0, slash));
                    max_index = std::max(max_index, index);
                }
            }
        }
        if (vertex_count == 0 || face_count == 0 || max_index > mesh.positions.size())
        {
            std::cerr << "self-check failed: vertices=" << vertex_count
                      << " faces=" << face_count << "\n";
            return 1;
        }

        std::cout << "vertices=" << mesh.positions.size()
                  << " faces=" << mesh.triangles.size()
                  << " uv_domain=[" << u_begin << "," << u_end << "]x["
                  << v_begin << "," << v_end << "]\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "error: " << error.what() << "\n";
        return 1;
    }
}
