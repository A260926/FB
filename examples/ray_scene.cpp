/**
 * Render a non-planar rational NURBS surface (a quarter-cylinder band)
 * directly from RaySurfaceScene ray queries, without any triangulation.
 * Orthographic rays run perpendicular to the cylinder axis, so the band
 * projects onto a two-dimensional silhouette. Hit pixels are shaded by the
 * Su x Sv unit normal, missed pixels keep a background colour. The output is
 * a binary 64x64 P3 PPM whose path is given on the command line.
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include <tinynurbs/tinynurbs.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

namespace
{
using tinynurbs::RationalSurface3d;

RationalSurface3d makeQuarterCylinder()
{
    // Rational quadratic quarter circle (unit radius) in the xy plane,
    // extruded along z into a band of height 2.
    const double w = std::sqrt(0.5);
    RationalSurface3d srf;
    srf.degree_u = 2;
    srf.degree_v = 1;
    srf.knots_u = {0, 0, 0, 1, 1, 1};
    srf.knots_v = {0, 0, 1, 1};
    srf.control_points = tinynurbs::array2<glm::dvec3>(
        3, 2, {glm::dvec3(1, 0, 0), glm::dvec3(1, 0, 2), glm::dvec3(1, 1, 0),
               glm::dvec3(1, 1, 2), glm::dvec3(0, 1, 0), glm::dvec3(0, 1, 2)});
    srf.weights = tinynurbs::array2<double>(3, 2, {1, 1, w, w, 1, 1});
    return srf;
}

int clampByte(double v)
{
    int x = static_cast<int>(std::lround(255.0 * v));
    return std::max(0, std::min(255, x));
}
} // namespace

int main(int argc, char **argv)
{
    if (argc != 2)
    {
        std::fprintf(stderr, "usage: %s output.ppm\n", argv[0]);
        return 2;
    }

    tinynurbs::RaySurfaceScene scene(std::vector<RationalSurface3d>{makeQuarterCylinder()});

    constexpr int N = 64;
    // Camera plane is the yz plane. Frames extend beyond the band's
    // [0,1] x [0,2] silhouette so background pixels surround it.
    const double y_lo = -0.3, y_hi = 1.3;
    const double z_lo = -0.3, z_hi = 2.3;

    std::FILE *out = std::fopen(argv[1], "wb");
    if (!out)
    {
        std::fprintf(stderr, "cannot open %s\n", argv[1]);
        return 2;
    }
    std::fprintf(out, "P3\n%d %d\n255\n", N, N);

    int hit_pixels = 0;
    int background_pixels = 0;
    tinynurbs::RayQueryOptions options;
    options.position_tolerance = 1e-8;
    options.parameter_tolerance = 1e-9;
    options.box_budget = 100000;

    for (int row = 0; row < N; ++row)
    {
        for (int col = 0; col < N; ++col)
        {
            double y = y_lo + (y_hi - y_lo) * (col + 0.5) / N;
            double z = z_hi - (z_hi - z_lo) * (row + 0.5) / N;
            // Rays run along +x, deliberately with a non-unit direction.
            auto result = scene.intersect(glm::dvec3(-5.0, y, z), glm::dvec3(2.0, 0, 0),
                                          0.0, 100.0, options);
            int r = 20, g = 24, b = 38; // background
            if (!result.complete)
            {
                std::fclose(out);
                std::fprintf(stderr, "incomplete query at pixel %d,%d\n", col, row);
                return 3;
            }
            if (!result.hits.empty())
            {
                const auto &n = result.hits.front().normal; // Su x Sv unit normal
                r = clampByte(0.5 * (n.x + 1.0));
                g = clampByte(0.5 * (n.y + 1.0));
                b = clampByte(0.5 * (n.z + 1.0));
                ++hit_pixels;
            }
            else
            {
                ++background_pixels;
            }
            std::fprintf(out, "%d %d %d\n", r, g, b);
        }
    }
    std::fclose(out);

    if (hit_pixels == 0 || background_pixels == 0)
    {
        std::fprintf(stderr, "expected both hit and background pixels, got hits=%d bg=%d\n",
                     hit_pixels, background_pixels);
        return 4;
    }
    std::printf("ray_scene render: %dx%d, hit_pixels=%d background_pixels=%d -> %s\n", N, N,
                hit_pixels, background_pixels, argv[1]);
    return 0;
}
