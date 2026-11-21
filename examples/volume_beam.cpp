// Volumetric beam example: a long, thin participating-medium slab lit by a
// point light just outside its near end. The bright peak next to the light is
// resolved by the optical/equiangular distance mixture instead of uniform
// distance sampling. Everything goes through the production volume_cast_ray
// entry; only the C++17 standard library is used.
#include "renderer.h"

#include <cmath>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

using namespace volr;

namespace {

RGB to_byte_scale(RGB c) {
    // Fixed linear scale (no per-image filtering / normalization).
    const double exposure = 0.35;
    for (int i = 0; i < 3; ++i) {
        double v = c[i] * exposure;
        if (!(v >= 0.0)) v = 0.0;
        if (v > 1.0) v = 1.0;
        c[i] = v;
    }
    return c;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: " << argv[0] << " <output-directory>\n";
        return 1;
    }
    const std::string out_path = std::string(argv[1]) + "/beam.ppm";

    Scene scene;
    scene.checkerboard = false;
    scene.background = RGB{0.01, 0.012, 0.02};

    // Two thin elongated boxes form the beam along the view axis. Their
    // coefficients overlap on a short stretch, where they are summed.
    scene.media.push_back(VolumeBox{{-0.35, -0.35, -17.0}, {0.35, 0.35, -9.0},
                                    RGB{0.03, 0.06, 0.10}, RGB{0.55, 0.65, 0.80}});
    scene.media.push_back(VolumeBox{{-0.35, -0.35, -10.0}, {0.35, 0.35, -4.2},
                                    RGB{0.02, 0.05, 0.09}, RGB{0.45, 0.60, 0.90}});

    // Point light just outside the near end of the beam: uniform distance
    // sampling would miss this sharp peak, equiangular sampling targets it.
    scene.lights.push_back(PointLight{{0.6, 0.4, -3.4}, RGB{90.0, 85.0, 75.0}});
    scene.lights.push_back(PointLight{{-3.0, 2.5, -6.0}, RGB{8.0, 10.0, 14.0}});

    // A diffuse sphere behind the beam and a mirror sphere to the side keep
    // the original surface paths (Phong, reflection) in the production render.
    Material diffuse{1.0, {1.4, 0.2, 0.0, 0.0}, RGB{0.35, 0.15, 0.12}, 12.0};
    Material mirror {1.0, {0.0, 16.0, 0.9, 0.0}, RGB{1.0, 1.0, 1.0}, 1425.0};
    scene.spheres.push_back(Sphere{{0.0, -0.2, -19.5}, 1.6, diffuse});
    scene.spheres.push_back(Sphere{{2.6, 0.4, -12.0}, 1.1, mirror});

    validate_scene(scene);

    constexpr int width = 320, height = 240;
    constexpr double fov = 0.9;
    constexpr int samples_per_segment = 6;
    std::vector<RGB> framebuffer(static_cast<size_t>(width) * height);

    RenderOptions opt;
    opt.mode = SampleMode::Mixed;
    opt.samples_per_segment = samples_per_segment;

    VolBudget total_budget;
    long long total_samples = 0, total_shadow = 0;
    for (int pix = 0; pix < width * height; ++pix) {
        double dir_x = (pix % width + 0.5) - width / 2.0;
        double dir_y = -(pix / width + 0.5) + height / 2.0;
        double dir_z = -height / (2.0 * std::tan(fov / 2.0));
        Vec3 dir = Vec3{dir_x, dir_y, dir_z}.normalized();

        opt.seed = 0x5eed0000ull ^ (static_cast<uint64_t>(pix) * 0x9e3779b97f4a7c15ull);
        VolBudget budget;
        framebuffer[pix] = volume_cast_ray(scene, {0, 0, 0}, dir, opt, &budget);
        total_samples += budget.volume_samples;
        total_shadow += budget.volume_shadow_queries;
    }

    std::ofstream ofs(out_path, std::ios::binary);
    if (!ofs) {
        std::cerr << "cannot open " << out_path << "\n";
        return 1;
    }
    ofs << "P6\n" << width << " " << height << "\n255\n";
    for (RGB c : framebuffer) {
        c = to_byte_scale(c);
        for (int chan = 0; chan < 3; ++chan)
            ofs << static_cast<char>(static_cast<int>(255.0 * c[chan] + 0.5));
    }
    std::cout << "BEAM_RENDER pixels=" << width * height
              << " samples_per_segment=" << samples_per_segment
              << " volume_samples=" << total_samples
              << " volume_shadow_queries=" << total_shadow
              << " out=" << out_path << "\n";
    return 0;
}
