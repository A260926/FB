// Renders a 48x32 P3 PPM preview of a scene made of two slanted plane groups. Pixels are colored
// by the original triangle identifier and true shading, and are produced through the oriented-BVH
// traversal. Usage: obb_preview <output.ppm>

#include "obb_scene.h"

#include <bvh/v2/default_builder.h>
#include <bvh/v2/node.h>

#include <cstdio>
#include <string>
#include <vector>

int main(int argc, char** argv) {
    const std::string output = argc > 1 ? argv[1] : "obb.ppm";

    using Node = bvh::v2::Node<obb_preview::Scalar, 3>;
    using BBox = bvh::v2::BBox<obb_preview::Scalar, 3>;
    using Vec3 = obb_preview::Vec3;

    auto scene = obb_preview::make_scene();
    std::vector<BBox> bboxes;
    std::vector<Vec3> centers;
    bboxes.reserve(scene.triangles.size());
    centers.reserve(scene.triangles.size());
    for (const auto& tri : scene.triangles) {
        bboxes.push_back(tri.get_bbox());
        centers.push_back(tri.get_center());
    }

    auto source = bvh::v2::DefaultBuilder<Node>::build(bboxes, centers);
    auto bvh = obb_preview::ObbBvh::from_bvh(source, scene.triangles);

    static constexpr std::uint32_t width = 48;
    static constexpr std::uint32_t height = 32;
    auto image = obb_preview::render_obb(scene, bvh, width, height);
    if (!obb_preview::write_ppm(image, output)) {
        std::fprintf(stderr, "cannot write '%s'\n", output.c_str());
        return 1;
    }

    // Cheap sanity check: the image must not be uniform.
    auto first = image.rgb[0];
    bool uniform = true;
    for (auto value : image.rgb) {
        if (value != first) {
            uniform = false;
            break;
        }
    }
    return uniform ? 2 : 0;
}
