/* Windowless thick-glass preview built on the production refraction path.
 *
 * The scene is generated procedurally: an opaque checkered background plane
 * and a closed convex glass wedge.  The camera sits outside the glass; the
 * preview captures real triangles and composes one transmitted ray per pixel
 * (air -> glass -> air -> background).
 *
 * Usage: refraction_example [output.ppm]
 */

#include "src/refraction.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace {

using namespace gfx::refraction;

/* --- procedural scene ------------------------------------------------- */

auto AddTriangle(Mesh& mesh, const Vec3& a, const Vec3& b, const Vec3& c,
                 const Vec3& normal, const Vec3& color) -> void {
    Triangle tri;
    tri.vertex[0].position = a;
    tri.vertex[1].position = b;
    tri.vertex[2].position = c;
    /* Match the winding to the outward normal. */
    if (Dot(Cross(b - a, c - a), normal) < 0.0)
        std::swap(tri.vertex[1], tri.vertex[2]);
    for (int i = 0; i < 3; ++i) {
        tri.vertex[i].normal = normal;
        tri.vertex[i].color = color;
    }
    mesh.triangles.push_back(tri);
}

auto AddQuad(Mesh& mesh, const Vec3& a, const Vec3& b, const Vec3& c,
             const Vec3& d, const Vec3& normal, const Vec3& color) -> void {
    AddTriangle(mesh, a, b, c, normal, color);
    AddTriangle(mesh, a, c, d, normal, color);
}

/* A convex wedge: triangular prism between two parallel triangular faces. */
auto MakeGlassWedge(double z_front, double z_back, double scale) -> Mesh {
    Mesh mesh;
    const Vec3 a{-1.0 * scale, -0.7 * scale, z_front};
    const Vec3 b{1.0 * scale, -0.7 * scale, z_front};
    const Vec3 c{0.0, 0.9 * scale, z_front};
    const Vec3 a2{-1.0 * scale, -0.7 * scale, z_back};
    const Vec3 b2{1.0 * scale, -0.7 * scale, z_back};
    const Vec3 c2{0.0, 0.9 * scale, z_back};
    const Vec3 white{1.0, 1.0, 1.0};
    AddTriangle(mesh, a, b, c, Vec3{0, 0, 1}, white);
    AddTriangle(mesh, a2, b2, c2, Vec3{0, 0, -1}, white);
    AddQuad(mesh, a, b, b2, a2, Vec3{0, -1, 0}, white);
    AddQuad(mesh, b, c, c2, b2, Normalize(Vec3{1.6, 1.0, 0.0}), white);
    AddQuad(mesh, c, a, a2, c2, Normalize(Vec3{-1.6, 1.0, 0.0}), white);
    return mesh;
}

/* Checkerboard backdrop: a grid of quads with two alternating linear
 * colours, built as real triangles at a fixed depth. */
auto MakeCheckerBackdrop(double z, double extent, int tiles) -> Mesh {
    Mesh mesh;
    const Vec3 light{0.75, 0.70, 0.55};
    const Vec3 dark{0.10, 0.25, 0.45};
    const double step = 2.0 * extent / tiles;
    for (int i = 0; i < tiles; ++i) {
        for (int j = 0; j < tiles; ++j) {
            const double x0 = -extent + i * step;
            const double y0 = -extent + j * step;
            const double x1 = x0 + step;
            const double y1 = y0 + step;
            const Vec3 color = ((i + j) % 2) ? light : dark;
            AddQuad(mesh, Vec3{x0, y0, z}, Vec3{x1, y0, z},
                    Vec3{x1, y1, z}, Vec3{x0, y1, z}, Vec3{0, 0, 1}, color);
        }
    }
    return mesh;
}

/* --- camera ----------------------------------------------------------- */

auto LookRotation(const Vec3& from, const Vec3& target, const Vec3& up,
                  Mat4& world_to_view) -> void {
    const Vec3 forward = Normalize(target - from);
    const Vec3 right = Normalize(Cross(forward, up));
    const Vec3 real_up = Cross(right, forward);
    world_to_view = Mat4::Identity();
    /* Rows of the rotation part are the camera axes expressed in world. */
    world_to_view(0, 0) = right.x;
    world_to_view(0, 1) = right.y;
    world_to_view(0, 2) = right.z;
    world_to_view(1, 0) = real_up.x;
    world_to_view(1, 1) = real_up.y;
    world_to_view(1, 2) = real_up.z;
    world_to_view(2, 0) = -forward.x;
    world_to_view(2, 1) = -forward.y;
    world_to_view(2, 2) = -forward.z;
    /* Translation: t = -R * from. */
    for (int row = 0; row < 3; ++row) {
        world_to_view(row, 3) =
            -(world_to_view(row, 0) * from.x +
              world_to_view(row, 1) * from.y +
              world_to_view(row, 2) * from.z);
    }
}

}  // namespace

int main(int argc, char** argv) {
    const std::string output = argc > 1 ? argv[1] : "refraction.ppm";

    constexpr int kWidth = 640;
    constexpr int kHeight = 400;
    constexpr double kNear = 0.5;
    constexpr double kFar = 100.0;

    const Vec3 camera{0.0, 0.0, 0.0};
    const Vec3 target{0.15, 0.05, -8.0};
    Mat4 world_to_view;
    LookRotation(camera, target, Vec3{0, 1, 0}, world_to_view);
    /* Normals use the same rotation (the model transform is the identity, so
     * the inverse transpose equals the rotation). */
    Mat4 normal_to_view = world_to_view;
    for (int row = 3; row < 4; ++row)
        for (int col = 0; col < 4; ++col) normal_to_view(row, col) = 0.0;
    normal_to_view(3, 3) = 1.0;

    /* Asymmetric, non-square frustum. */
    const double aspect = static_cast<double>(kWidth) / kHeight;
    const double tan_half = std::tan(0.62 * 0.5);
    const double left = -tan_half * aspect * kNear;
    const double right = tan_half * aspect * kNear;
    const double bottom = -tan_half * kNear;
    const double top = tan_half * kNear;
    const Mat4 projection =
        PerspectiveProjection(left, right, bottom, top, kNear, kFar);

    CaptureSettings settings;
    settings.width = kWidth;
    settings.height = kHeight;
    settings.world_to_view = world_to_view;
    settings.normal_to_view = normal_to_view;
    settings.projection = projection;
    settings.near_depth = kNear;
    settings.far_depth = kFar;

    const Mesh glass = MakeGlassWedge(-6.0, -8.4, 1.35);
    const Mesh background = MakeCheckerBackdrop(-16.0, 14.0, 12);

    SceneCapture scene;
    std::string error;
    if (!CaptureScene(background, glass, settings, scene, &error)) {
        std::fprintf(stderr, "capture failed: %s\n", error.c_str());
        return 1;
    }

    Material material;
    material.ior = 1.47;
    material.sigma_a = {0.18, 0.09, 0.04};
    material.environment = {0.05, 0.07, 0.10};

    CompositeResult result;
    if (!Compose(scene, material, result, &error)) {
        std::fprintf(stderr, "compose failed: %s\n", error.c_str());
        return 1;
    }

    /* Report a few pixel traces so the example is self-checking. */
    const std::size_t centre =
        static_cast<std::size_t>(kHeight / 2) * kWidth + kWidth / 2;
    const PixelTrace& trace = result.pixels[centre];
    std::printf(
        "compose ok: %dx%d, centre entrance=%d exit=%d background=%d "
        "internal=%.4f\n",
        result.width, result.height, trace.entrance_visible ? 1 : 0,
        trace.exit_found ? 1 : 0, trace.background_found ? 1 : 0,
        trace.exit_hit.distance);

    if (!WritePPM(output, result, &error)) {
        std::fprintf(stderr, "write failed: %s\n", error.c_str());
        return 1;
    }
    std::printf("wrote %s\n", output.c_str());
    return 0;
}
