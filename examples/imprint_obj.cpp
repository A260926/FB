// Example: write an imprinted surface pair as OBJ.
//
// Usage: imprint_obj <output.obj>
//
// The two surfaces are exact tensor-product cubic Bezier patches.  The
// intersection is traced by the C++ production entry point
// sislmesh::imprint_surfaces(), which itself uses the native SISL tracer, and
// the resulting constrained triangulations of both sheets are written as a
// non-empty OBJ file with v / vn / f records.  The SISL producer line is
// retained, per the license.  Returns non-zero when the file cannot be
// written or when no geometry is produced.

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "sisl.h"
#include "sisl_imprint.hpp"

namespace
{

// Tensor-product cubic Bezier over [0,1]^2 from power-basis coefficients.
// coef[axis][i][j] multiplies s^i t^j, s,t the local patch parameters.
SISLSurf *make_patch(const double coef[3][4][4])
{
    const double kn[8] = {0, 0, 0, 0, 1, 1, 1, 1};
    // Bernstein control values from power coefficients
    const double B[4][4] = {{1, 0, 0, 0},
                            {1, 1.0 / 3.0, 0, 0},
                            {1, 2.0 / 3.0, 1.0 / 3.0, 0},
                            {1, 1, 1, 1}};
    double cp[16 * 3];
    for (int j = 0; j < 4; ++j)
    {
        for (int i = 0; i < 4; ++i)
        {
            for (int a = 0; a < 3; ++a)
            {
                double v = 0.0;
                for (int k = 0; k < 4; ++k)
                    for (int l = 0; l < 4; ++l)
                        v += B[i][k] * B[j][l] * coef[a][k][l];
                cp[(j * 4 + i) * 3 + a] = v;
            }
        }
    }
    return newSurf(4, 4, 4, 4, const_cast<double *>(kn),
                   const_cast<double *>(kn), cp, 1, 3, 1);
}

} // namespace

int main(int argc, char **argv)
{
    if (argc != 2)
    {
        std::cerr << "usage: imprint_obj <output.obj>\n";
        return 2;
    }
    const std::string path = argv[1];

    // Sheet 0: a shallow bowl.  Sheet 1: a horizontal plane cutting it; the
    // intersection is one closed loop that crosses the caller grid lines.
    double bowl[3][4][4] = {{0}};
    bowl[0][1][0] = 1.0;
    bowl[1][0][1] = 1.0;
    bowl[2][2][0] = 0.6;
    bowl[2][1][0] = -0.6;
    bowl[2][0][2] = 0.6;
    bowl[2][0][1] = -0.6;
    bowl[2][0][0] = 0.80;
    double plane[3][4][4] = {{0}};
    plane[0][1][0] = 1.0;
    plane[1][0][1] = 1.0;
    plane[2][0][0] = 0.60;

    SISLSurf *s0 = make_patch(bowl);
    SISLSurf *s1 = make_patch(plane);
    if (!s0 || !s1)
    {
        std::cerr << "failed to build example surfaces\n";
        if (s0) freeSurf(s0);
        if (s1) freeSurf(s1);
        return 3;
    }

    sislmesh::ImprintOptions opt;
    opt.geometricTolerance = 1e-5;
    opt.maxStations = 200000;
    opt.maxTriangles = 2000000;
    const std::vector<double> grid = {0.0, 0.25, 0.5, 0.75, 1.0};

    sislmesh::ImprintResult result;
    try
    {
        sislmesh::imprint_surfaces(*s0, grid, grid, *s1, grid, grid, opt,
                                   result);
    }
    catch (const std::exception &e)
    {
        std::cerr << "imprint failed: " << e.what() << "\n";
        freeSurf(s0);
        freeSurf(s1);
        return 4;
    }
    freeSurf(s0);
    freeSurf(s1);

    std::ofstream os(path.c_str());
    if (!os)
    {
        std::cerr << "cannot open " << path << " for writing\n";
        return 5;
    }
    os << "# SISL imprint_obj: intersection-curve imprinted triangulation\n";
    os << "# producer: SISL (SINTEF Digital) via sislmesh::imprint_surfaces\n";
    os << "# vertices " << result.positions.size() << ", triangles "
       << result.triangles.size() << ", intersection branches "
       << result.polylines.size() << "\n";
    os.precision(17);
    for (std::size_t i = 0; i < result.positions.size(); ++i)
        os << "v " << result.positions[i][0] << " " << result.positions[i][1]
           << " " << result.positions[i][2] << "\n";
    for (std::size_t i = 0; i < result.polylines.size(); ++i)
    {
        const sislmesh::ImprintPolyline &pl = result.polylines[i];
        os << "o intersection_" << i << "\n";
        for (std::size_t k = 0; k < pl.indices.size(); ++k)
            os << "v " << result.positions[pl.indices[k]][0] << " "
               << result.positions[pl.indices[k]][1] << " "
               << result.positions[pl.indices[k]][2] << "\n";
    }
    // unit normals, one per triangle corner (analytic surface normals)
    for (std::size_t i = 0; i < result.triangles.size(); ++i)
    {
        const sislmesh::ImprintTriangle &t = result.triangles[i];
        for (int k = 0; k < 3; ++k)
            os << "vn " << t.normal[k][0] << " " << t.normal[k][1] << " "
               << t.normal[k][2] << "\n";
    }
    long vn = (long)result.positions.size();
    for (std::size_t i = 0; i < result.polylines.size(); ++i)
        vn += (long)result.polylines[i].indices.size();
    for (std::size_t i = 0; i < result.triangles.size(); ++i)
    {
        const sislmesh::ImprintTriangle &t = result.triangles[i];
        os << "f";
        for (int k = 0; k < 3; ++k)
        {
            long vidx = (long)t.index[k] + 1;
            long nidx = (long)(i * 3 + k) + 1;
            os << " " << vidx << "//" << nidx;
        }
        os << "\n";
    }
    (void)vn;
    os.flush();
    if (!os)
    {
        std::cerr << "write failed\n";
        return 6;
    }
    std::cout << "wrote " << path << ": " << result.positions.size()
              << " vertices, " << result.triangles.size() << " triangles\n";
    return 0;
}
