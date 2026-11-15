// Example: build a rational surface on a non-unit parameter domain, run it
// through the original knot-insertion / degree-elevation editing path and a
// file round trip, then export it as an OBJ mesh with BuildSurfaceMesh.
//
// Usage: domain_mesh <output.obj>

#include <cmath>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include <libnurbs/Surface/Surface.hpp>
#include <libnurbs/Surface/SurfaceMesh.hpp>

using namespace libnurbs;

namespace
{
    // Rational cylinder band on U = [-3, 5], V = [10, 12]:
    // (R cos t, R sin t, v) with t sweeping [0.2, 1.4] as u sweeps the domain.
    Surface MakeRationalSurface()
    {
        constexpr double R   = 2.0;
        constexpr double TH0 = 0.2;
        constexpr double TH1 = 1.4;
        Surface s;
        s.DegreeU = 2;
        s.DegreeV = 1;
        s.KnotsU  = KnotVector{{-3, -3, -3, 5, 5, 5}};
        s.KnotsV  = KnotVector{{10, 10, 12, 12}};
        const double thm  = 0.5 * (TH0 + TH1);
        const double half = 0.5 * (TH1 - TH0);
        const double w1   = std::cos(half);
        s.ControlPoints   = ControlPointGrid(3, 2);
        const double zs[2] = {10, 12};
        for (int j = 0; j < 2; ++j)
        {
            s.ControlPoints.Get(0, j) = Vec4(R * std::cos(TH0), R * std::sin(TH0), zs[j], 1.0);
            s.ControlPoints.Get(1, j) = Vec4(R / w1 * std::cos(thm), R / w1 * std::sin(thm), zs[j], w1);
            s.ControlPoints.Get(2, j) = Vec4(R * std::cos(TH1), R * std::sin(TH1), zs[j], 1.0);
        }
        return s;
    }
}

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        std::cerr << "usage: domain_mesh <output.obj>\n";
        return 2;
    }
    try
    {
        // Non-unit-domain rational surface through the original editing path.
        Surface surface = MakeRationalSurface();
        surface         = surface.InsertKnotU(0.0).InsertKnotV(11.0).ElevateDegreeV();

        // File round trips (text and binary) through the original serializers.
        for (bool binary : {false, true})
        {
            std::stringstream io(std::ios::in | std::ios::out | std::ios::binary);
            surface.SaveToFile(io, binary);
            io.seekg(0);
            Surface back;
            back.LoadFromFile(io);
            if ((back.Evaluate(1.25, 10.8) - surface.Evaluate(1.25, 10.8)).norm() > 1e-9)
            {
                std::cerr << "file round trip changed the geometry\n";
                return 3;
            }
            surface = back;
        }

        // Original-domain stations; internal knots are merged in by the builder.
        const std::vector<double> us{-3, -1, 2.5, 5};
        const std::vector<double> vs{10, 11.5, 12};
        SurfaceMesh mesh = BuildSurfaceMesh(surface, us, vs);
        if (mesh.positions.empty() || mesh.triangles.empty())
        {
            std::cerr << "empty mesh\n";
            return 4;
        }

        // Write OBJ: positions as v, per-corner original UV as vt, per-corner
        // one-sided analytic unit normal as vn, faces as v/vt/vn triples.
        {
            std::ofstream out(argv[1]);
            if (!out.is_open())
            {
                std::cerr << "cannot open output file: " << argv[1] << "\n";
                return 5;
            }
            out << std::fixed;
            out.precision(17);
            for (const auto& p : mesh.positions)
                out << "v " << p.x() << ' ' << p.y() << ' ' << p.z() << '\n';
            for (const auto& tri_uv : mesh.corner_uv)
                for (const auto& uv : tri_uv)
                    out << "vt " << uv.x() << ' ' << uv.y() << '\n';
            for (const auto& tri_n : mesh.corner_normals)
                for (const auto& n : tri_n)
                    out << "vn " << n.x() << ' ' << n.y() << ' ' << n.z() << '\n';
            for (size_t t = 0; t < mesh.triangles.size(); ++t)
            {
                out << 'f';
                for (int k = 0; k < 3; ++k)
                {
                    const size_t corner = 3 * t + k + 1; // vt/vn are per corner
                    out << ' ' << mesh.triangles[t][k] + 1 << '/' << corner << '/' << corner;
                }
                out << '\n';
            }
        }

        // Self-check the written file: counts, parameter/position consistency,
        // unit normals and on-surface positions.
        size_t nv = 0, nvt = 0, nvn = 0, nf = 0;
        std::vector<Vec3> obj_v;
        std::vector<SurfaceMesh::UV> obj_vt;
        std::vector<Vec3> obj_vn;
        std::vector<std::array<int, 3>> obj_f;
        {
            std::ifstream in(argv[1]);
            if (!in.is_open())
            {
                std::cerr << "cannot reopen output file\n";
                return 6;
            }
            std::string line;
            while (std::getline(in, line))
            {
                std::stringstream ss(line);
                std::string tag;
                ss >> tag;
                if (tag == "v")
                {
                    Vec3 p;
                    ss >> p.x() >> p.y() >> p.z();
                    obj_v.push_back(p);
                    ++nv;
                }
                else if (tag == "vt")
                {
                    SurfaceMesh::UV uv;
                    ss >> uv.x() >> uv.y();
                    obj_vt.push_back(uv);
                    ++nvt;
                }
                else if (tag == "vn")
                {
                    Vec3 n;
                    ss >> n.x() >> n.y() >> n.z();
                    obj_vn.push_back(n);
                    ++nvn;
                }
                else if (tag == "f")
                {
                    std::array<int, 3> face{};
                    for (int k = 0; k < 3; ++k)
                    {
                        std::string token;
                        ss >> token;
                        face[k] = std::stoi(token.substr(0, token.find('/')));
                    }
                    obj_f.push_back(face);
                    ++nf;
                }
            }
        }
        if (nv != mesh.positions.size() || nf != mesh.triangles.size() ||
            nvt != 3 * nf || nvn != 3 * nf || nv == 0 || nf == 0)
        {
            std::cerr << "obj self-check failed: bad counts\n";
            return 7;
        }
        for (size_t t = 0; t < nf; ++t)
            for (int k = 0; k < 3; ++k)
            {
                const Vec3& p  = obj_v[obj_f[t][k] - 1];
                const auto& uv = obj_vt[3 * t + k];
                const Vec3& n  = obj_vn[3 * t + k];
                if (std::abs(n.norm() - 1.0) > 1e-9)
                {
                    std::cerr << "obj self-check failed: non-unit normal\n";
                    return 8;
                }
                if ((surface.Evaluate(uv.x(), uv.y()) - p).norm() > 1e-8)
                {
                    std::cerr << "obj self-check failed: position off surface\n";
                    return 9;
                }
            }

        std::cout << "vertices=" << nv << " faces=" << nf << '\n';
        return 0;
    }
    catch (const std::exception& e)
    {
        std::cerr << "error: " << e.what() << '\n';
        return 1;
    }
}
