// Example: opaque CPU surfel render over a real triangle base image.
//
// Usage: surfels <output.ppm>
//
// Draws an opaque triangle backdrop with the shared triangle rasterizer, then
// overlays coloured disks (each with two independent tangent half-axes) that
// intersect at oblique angles.  The result is written as a binary PPM whose
// row order is flipped (PPM is top-down, the framebuffer is y-up).
//
// Everything below is expressed in world space.  The camera sits on +Z looking
// down -Z; the view matrix is a pure rigid transform.

#include <array>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <string>
#include <vector>

#include "src/surfels.h"

using namespace gfx;

namespace {

constexpr int kWidth = 640;
constexpr int kHeight = 400;
constexpr double kNear = 1.0;
constexpr double kFar = 100.0;

auto Perspective(double near_d, double far_d, double l, double r, double b, double t) -> Mat4d {
    Mat4d m;
    m(0, 0) = 2 * near_d / (r - l);
    m(0, 2) = (r + l) / (r - l);
    m(1, 1) = 2 * near_d / (t - b);
    m(1, 2) = (t + b) / (t - b);
    m(2, 2) = -(far_d + near_d) / (far_d - near_d);
    m(2, 3) = -2 * far_d * near_d / (far_d - near_d);
    m(3, 2) = -1;
    return m;
}

// Rigid view matrix: rotate the world about Y by `yaw`, then translate the
// camera to (0, 0, eye_z).  Result maps world points to view space.
auto ViewMatrix(double yaw, double eye_z) -> Mat4d {
    Mat4d m = Mat4d::Identity();
    const double c = std::cos(yaw), s = std::sin(yaw);
    // R = Ry(-yaw) (world rotates by +yaw in front of the camera).
    m(0, 0) = c;  m(0, 2) = -s;
    m(2, 0) = s;  m(2, 2) = c;
    // view = R * (world - eye); eye = (0,0,eye_z).
    m(0, 3) = 0.0;
    m(1, 3) = 0.0;
    m(2, 3) = -eye_z;
    return m;
}

auto MakeSurfel(double px, double py, double pz, double ax, double ay, double az,
                double bx, double by, double bz, Color c) -> Surfel {
    Surfel s;
    s.p = {px, py, pz};
    s.a = {ax, ay, az};
    s.b = {bx, by, bz};
    s.color = c;
    return s;
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "Usage: %s <output.ppm>\n", argv[0]);
        return 1;
    }

    SurfelCamera camera;
    camera.view = ViewMatrix(0.22, 6.0);
    // ~53 degree vertical FOV, 1.7:1 aspect; asymmetric-free but finite.
    camera.projection = Perspective(kNear, kFar, -1.7, 1.7, -1.0, 1.0);
    camera.near_depth = kNear;
    camera.far_depth = kFar;
    // View-space direction towards the light; it faces the camera so the disks
    // (which face the camera) are well lit.
    camera.light_direction = {0.35, 0.40, 0.85};

    // --- Opaque triangle backdrop -----------------------------------------
    // A large back wall at world z = -2.0, built from two triangles.  Normal
    // faces +Z (towards the camera and light).
    Target target(kWidth, kHeight);
    target.Reset();
    const Vec n({0, 0, 1, 0});
    RasterizeTriangle(camera.projection, camera.view, Mat4d::Identity(),
                      Vec({-8.0, -5.0, -2.0, 1}), Vec({8.0, -5.0, -2.0, 1}),
                      Vec({8.0, 5.0, -2.0, 1}), n, n, n, &target);
    RasterizeTriangle(camera.projection, camera.view, Mat4d::Identity(),
                      Vec({-8.0, -5.0, -2.0, 1}), Vec({8.0, 5.0, -2.0, 1}),
                      Vec({-8.0, 5.0, -2.0, 1}), n, n, n, &target);

    BaseImage base = BaseFromTarget(target);

    // --- Obliquely intersecting coloured disks ----------------------------
    // All in front of the wall (z in (-2, 0)).  Each has non-uniform half-axes
    // and a different tilt, so depth ordering flips per pixel where they
    // overlap.
    std::vector<Surfel> surfels;
    surfels.push_back(MakeSurfel(-0.90, 0.15, 0.10, 1.05, 0.08, 0.55,
                                 0.06, 0.72, -0.20, Color(0.95f, 0.25f, 0.20f)));
    surfels.push_back(MakeSurfel(0.10, -0.10, -0.10, 1.20, 0.16, -0.68,
                                 0.14, 0.62, 0.30, Color(0.20f, 0.75f, 0.95f)));
    surfels.push_back(MakeSurfel(1.00, 0.28, 0.20, 0.95, -0.12, 0.55,
                                 -0.06, 0.80, 0.22, Color(0.30f, 0.90f, 0.35f)));
    surfels.push_back(MakeSurfel(-0.25, 0.95, -0.30, 0.80, 0.16, 0.24,
                                 0.08, 0.58, -0.32, Color(0.95f, 0.85f, 0.25f)));
    surfels.push_back(MakeSurfel(0.60, -0.95, 0.15, 0.85, 0.08, -0.40,
                                 0.12, 0.50, 0.45, Color(0.80f, 0.35f, 0.95f)));

    SurfelOptions options;
    options.delta = 0.05;  // blend a thin band to soften reconstructions
    SurfelResult result;
    if (!RenderSurfels(surfels, camera, base, options, &result)) {
        std::fprintf(stderr, "RenderSurfels failed\n");
        return 1;
    }

    std::vector<uint8_t> ppm;
    if (!EncodePPM(result.color.data(), result.width, result.height, &ppm)) {
        std::fprintf(stderr, "EncodePPM failed\n");
        return 1;
    }
    std::FILE* f = std::fopen(argv[1], "wb");
    if (!f) {
        std::fprintf(stderr, "Cannot open %s\n", argv[1]);
        return 1;
    }
    const size_t written = std::fwrite(ppm.data(), 1, ppm.size(), f);
    std::fclose(f);
    if (written != ppm.size()) {
        std::fprintf(stderr, "Short write\n");
        return 1;
    }

    std::printf("wrote %s (%dx%d), %zu surfels, %zu pixels hit, %zu ray tests\n",
                argv[1], result.width, result.height, surfels.size(),
                result.stats.pixels_with_hits, result.stats.intersection_tests);
    return 0;
}
