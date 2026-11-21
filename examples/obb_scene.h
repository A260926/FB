#ifndef OBB_PREVIEW_SCENE_H
#define OBB_PREVIEW_SCENE_H

// Deterministic preview scene shared by examples/obb_preview.cpp and tests/obb_cases.cpp.
// The scene contains two groups of planes with clearly different slants, so that the fitted
// oriented boxes must use different frames for the two halves of the image. Rendering is done
// both through the production oriented-BVH traversal and through an exhaustive world-space
// triangle loop; comparing the two images per pixel is the cross-check used by the test suite.

#include <bvh/v2/obb.h>
#include <bvh/v2/tri.h>
#include <bvh/v2/vec.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace obb_preview {

using Scalar = float;
using Vec3 = bvh::v2::Vec<Scalar, 3>;
using Tri3 = bvh::v2::Tri<Scalar, 3>;
using ObbBvh = bvh::v2::OrientedBvh<Scalar>;

struct Scene {
    std::vector<Tri3> triangles;
};

struct Image {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<unsigned char> rgb; // 3 bytes per pixel

    Image() = default;
    Image(std::uint32_t w, std::uint32_t h)
        : width(w), height(h), rgb(static_cast<size_t>(w) * h * 3, 0)
    {}
};

inline Vec3 normalized(const Vec3& v) {
    return v * (1.0f / std::sqrt(bvh::v2::dot(v, v)));
}

/// Builds the shared preview scene: two grids of small quads. Group A is tilted around the x axis,
/// group B around the y axis. Triangle identifiers are their index in `Scene::triangles`.
inline Scene make_scene() {
    Scene scene;

    auto add_quad = [&] (const Vec3& center, const Vec3& u, const Vec3& v) {
        auto a = center - u - v;
        auto b = center + u - v;
        auto c = center + u + v;
        auto d = center - u + v;
        scene.triangles.emplace_back(a, b, c);
        scene.triangles.emplace_back(a, c, d);
    };

    // Group A: slanted about the x axis.
    auto axis_a = normalized(Vec3(0.0f, -0.6f, 0.8f));
    auto u_a = Vec3(1.0f, 0.0f, 0.0f);
    auto v_a = normalized(bvh::v2::cross(axis_a, u_a));
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            auto center = Vec3(-2.1f + 0.85f * static_cast<float>(i),
                               -0.9f + 0.85f * static_cast<float>(j),
                               0.15f + 0.01f * static_cast<float>(i * 3 + j));
            add_quad(center, u_a * 0.34f, v_a * 0.34f);
        }
    }

    // Group B: slanted about the y axis.
    auto axis_b = normalized(Vec3(-0.7f, 0.0f, 0.7f));
    (void)axis_b;
    auto u_b = Vec3(0.0f, 1.0f, 0.0f);
    auto v_b = normalized(bvh::v2::cross(axis_b, u_b));
    // The second group sits at a larger distance, so both halves of the image are visible.
    for (int i = 0; i < 2; ++i) {
        for (int j = 0; j < 2; ++j) {
            auto center = Vec3(1.35f + 0.72f * static_cast<float>(i),
                               -0.7f + 0.72f * static_cast<float>(j),
                               5.2f + 0.01f * static_cast<float>(i * 2 + j));
            add_quad(center, u_b * 0.32f, v_b * 0.32f);
        }
    }
    return scene;
}

inline Vec3 triangle_normal(const Tri3& tri) {
    return normalized(bvh::v2::cross(tri.p1 - tri.p0, tri.p2 - tri.p0));
}

/// Deterministic color for an original face identifier, in [0, 1].
inline Vec3 face_color(std::size_t face_id) {
    auto h = static_cast<float>(face_id) * 0.61803398875f;
    h -= std::floor(h);
    auto r = std::fabs(h * 6.0f - 3.0f) - 1.0f;
    auto g = 2.0f - std::fabs(h * 6.0f - 2.0f);
    auto b = 2.0f - std::fabs(h * 6.0f - 4.0f);
    return Vec3(
        std::clamp(r, 0.0f, 1.0f),
        std::clamp(g, 0.0f, 1.0f),
        std::clamp(b, 0.0f, 1.0f));
}

inline Vec3 shade(std::size_t face_id, const Tri3& tri) {
    auto light = normalized(Vec3(0.35f, 0.55f, 0.75f));
    auto lambert = std::max(0.0f, bvh::v2::dot(triangle_normal(tri), light));
    auto base = face_color(face_id);
    auto intensity = 0.22f + 0.78f * lambert;
    return Vec3(base[0] * intensity, base[1] * intensity, base[2] * intensity);
}

inline Vec3 background() {
    return Vec3(0.06f, 0.07f, 0.09f);
}

inline bvh::v2::Ray<Scalar, 3> make_pixel_ray(
    std::uint32_t px,
    std::uint32_t py,
    std::uint32_t width,
    std::uint32_t height)
{
    auto aspect = static_cast<float>(width) / static_cast<float>(height);
    auto x = ((static_cast<float>(px) + 0.5f) / static_cast<float>(width) * 2.0f - 1.0f) * 2.2f * aspect;
    auto y = (1.0f - (static_cast<float>(py) + 0.5f) / static_cast<float>(height) * 2.0f) * 2.2f;
    return bvh::v2::Ray<Scalar, 3>(Vec3(x, y, 8.0f), Vec3(0.0f, 0.0f, -1.0f), 0.0f, 100.0f);
}

inline unsigned char to_byte(float value) {
    auto clamped = std::clamp(value, 0.0f, 1.0f);
    return static_cast<unsigned char>(std::lround(clamped * 255.0f));
}

/// Renders the scene through the production oriented-BVH traversal.
inline Image render_obb(const Scene& scene, const ObbBvh& bvh, std::uint32_t width, std::uint32_t height) {
    Image image(width, height);
    for (std::uint32_t py = 0; py < height; ++py) {
        for (std::uint32_t px = 0; px < width; ++px) {
            auto ray = make_pixel_ray(px, py, width, height);
            auto hit_id = static_cast<std::size_t>(-1);
            auto best_t = std::numeric_limits<Scalar>::infinity();
            bvh.intersect_closest(ray, [&] (size_t begin, size_t end) {
                for (size_t i = begin; i < end; ++i) {
                    auto prim_id = bvh.prim_ids()[i];
                    if (auto hit = bvh::v2::PrecomputedTri<Scalar>(scene.triangles[prim_id]).intersect(ray)) {
                        auto t = std::get<0>(*hit);
                        // Deterministic tie-break by original primitive identifier so that the
                        // traversal order cannot change the shading of edge pixels. Equal
                        // distances are exact ties here, since both paths use the same
                        // triangle routine.
                        if (t < best_t || (t == best_t && prim_id < hit_id)) {
                            best_t = t;
                            ray.tmax = t;
                            hit_id = prim_id;
                        }
                    }
                }
            });
            auto color = hit_id == static_cast<std::size_t>(-1)
                ? background()
                : shade(hit_id, scene.triangles[hit_id]);
            auto* pixel = &image.rgb[(static_cast<size_t>(py) * width + px) * 3];
            pixel[0] = to_byte(color[0]);
            pixel[1] = to_byte(color[1]);
            pixel[2] = to_byte(color[2]);
        }
    }
    return image;
}

/// Renders the scene by testing every triangle with every ray.
inline Image render_exhaustive(const Scene& scene, std::uint32_t width, std::uint32_t height) {
    Image image(width, height);
    for (std::uint32_t py = 0; py < height; ++py) {
        for (std::uint32_t px = 0; px < width; ++px) {
            auto ray = make_pixel_ray(px, py, width, height);
            auto hit_id = static_cast<std::size_t>(-1);
            auto best_t = std::numeric_limits<Scalar>::infinity();
            for (size_t i = 0; i < scene.triangles.size(); ++i) {
                if (auto hit = bvh::v2::PrecomputedTri<Scalar>(scene.triangles[i]).intersect(ray)) {
                    auto t = std::get<0>(*hit);
                    if (t < best_t || (t == best_t && i < hit_id)) {
                        best_t = t;
                        ray.tmax = t;
                        hit_id = i;
                    }
                }
            }
            auto color = hit_id == static_cast<std::size_t>(-1)
                ? background()
                : shade(hit_id, scene.triangles[hit_id]);
            auto* pixel = &image.rgb[(static_cast<size_t>(py) * width + px) * 3];
            pixel[0] = to_byte(color[0]);
            pixel[1] = to_byte(color[1]);
            pixel[2] = to_byte(color[2]);
        }
    }
    return image;
}

inline bool write_ppm(const Image& image, const std::string& path) {
    auto* file = std::fopen(path.c_str(), "wb");
    if (!file)
        return false;
    std::fprintf(file, "P3\n%u %u\n255\n", image.width, image.height);
    for (std::uint32_t y = 0; y < image.height; ++y) {
        for (std::uint32_t x = 0; x < image.width; ++x) {
            auto* pixel = &image.rgb[(static_cast<size_t>(y) * image.width + x) * 3];
            std::fprintf(file, "%d %d %d\n", pixel[0], pixel[1], pixel[2]);
        }
    }
    std::fclose(file);
    return true;
}

} // namespace obb_preview

#endif
