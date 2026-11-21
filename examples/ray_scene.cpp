/**
 * Ray-trace a non-planar rational NURBS surface (a unit-radius quarter
 * cylinder band) directly against its parametric representation and write a
 * 64x64 binary PPM (P3) whose hit pixels are shaded by the Su x Sv normal.
 *
 * Usage: ray_scene <output.ppm>
 */

#include <tinynurbs/tinynurbs.h>

#include <cmath>
#include <cstdio>
#include <vector>

using tinynurbs::RationalSurface3d;
using tinynurbs::RaySurfaceScene;
using tinynurbs::RayOptions;
using glm::dvec3;

static RationalSurface3d makeQuarterCylinder()
{
    RationalSurface3d s;
    s.degree_u = 2;
    s.degree_v = 1;
    const double w = std::sqrt(0.5);
    s.knots_u = {0, 0, 0, 1, 1, 1};
    s.knots_v = {0, 0, 1, 1};
    s.control_points = {3, 2,
                        {dvec3(1, 0, 0), dvec3(1, 0, 2),
                         dvec3(1, 1, 0), dvec3(1, 1, 2),
                         dvec3(0, 1, 0), dvec3(0, 1, 2)}};
    s.weights = {3, 2, {1, 1, w, w, 1, 1}};
    return s;
}

int main(int argc, char **argv)
{
    if (argc != 2)
    {
        std::fprintf(stderr, "usage: %s <output.ppm>\n", argv[0]);
        return 2;
    }

    RaySurfaceScene scene({makeQuarterCylinder()});
    if (scene.status() != tinynurbs::RaySceneError::None) return 3;

    constexpr int kRes = 64;
    // Orthographic camera looking down -Y over an x-z window larger than the
    // cylinder, so the image contains both shaded surface and background.
    constexpr double kXMin = -0.5, kXMax = 1.5;
    constexpr double kZMin = -0.5, kZMax = 2.5;
    const dvec3 direction(0.0, -1.0, 0.0);

    RayOptions opt; // 1e-8 / 1e-9 / 100000 defaults
    std::vector<unsigned char> rgb(kRes * kRes * 3, 0);
    int hit_pixels = 0;
    int background_pixels = 0;

    for (int row = 0; row < kRes; ++row)
    {
        for (int col = 0; col < kRes; ++col)
        {
            const double fx = (col + 0.5) / kRes;
            const double fz = 1.0 - (row + 0.5) / kRes;
            const double x = kXMin + fx * (kXMax - kXMin);
            const double z = kZMin + fz * (kZMax - kZMin);
            const dvec3 origin(x, 5.0, z);
            auto result = scene.intersect(origin, direction, 0.0, 20.0, opt);
            if (!result.complete) return 4;
            int idx = (row * kRes + col) * 3;
            if (result.hits.empty())
            {
                // Near-black background with a faint marker so it is nonzero.
                rgb[idx] = 12;
                rgb[idx + 1] = 12;
                rgb[idx + 2] = 18;
                ++background_pixels;
                continue;
            }
            const auto &hit = result.hits.front(); // results are t-sorted
            const dvec3 n = hit.normal;
            rgb[idx] = static_cast<unsigned char>(255.0 * 0.5 * (n.x + 1.0));
            rgb[idx + 1] = static_cast<unsigned char>(255.0 * 0.5 * (n.y + 1.0));
            rgb[idx + 2] = static_cast<unsigned char>(255.0 * 0.5 * (n.z + 1.0));
            ++hit_pixels;
        }
    }

    if (hit_pixels == 0 || background_pixels == 0)
    {
        std::fprintf(stderr, "empty hit/background region: %d hits, %d background\n",
                     hit_pixels, background_pixels);
        return 1;
    }

    std::FILE *fp = std::fopen(argv[1], "wb");
    if (!fp)
    {
        std::fprintf(stderr, "cannot open %s\n", argv[1]);
        return 5;
    }
    std::fprintf(fp, "P3\n%d %d\n255\n", kRes, kRes);
    for (int i = 0; i < kRes * kRes; ++i)
    {
        std::fprintf(fp, "%d %d %d\n", rgb[3 * i], rgb[3 * i + 1], rgb[3 * i + 2]);
    }
    std::fclose(fp);
    std::printf("RAY_SCENE_PPM path=%s hits=%d background=%d\n", argv[1], hit_pixels,
                background_pixels);
    return 0;
}
