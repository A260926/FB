// Fiber preview: renders several bundles of thin open-cylinder fibers with
// different orientations and RGB absorption coefficients against a
// directional background, using the deterministic sample stream.
//
// Usage: fiber_preview <output_directory>
// Writes:
//   <dir>/fiber_preview.ppm       P6 image (gamma-encoded for display)
//   <dir>/fiber_preview_reps.txt  linear RGB of representative rays
// Everything here is C++17 standard library only.
#include "fiber_renderer.h"
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {

constexpr int    WIDTH = 320;
constexpr int    HEIGHT = 240;
constexpr double FOV = 1.05;
constexpr int    SPP = 48; // deterministic samples per pixel, fixed stream

dvec3 background_fn(const dvec3& wi) {
    // Dark cool base plus a sharp directional key band; the anisotropic
    // fiber lobes turn this into highlights stretched along the filaments.
    const dvec3 base{0.015, 0.02, 0.035};
    const dvec3 key_dir = dvec3{0.35, -0.45, 0.82}.normalized();
    const dvec3 key_color{6.0, 5.6, 4.8};
    double q = std::max(0.0, wi * key_dir);
    dvec3 col = base + key_color * (q * q * q * q * q * q * q * q);
    // A soft fill from below so shadow-side fibers keep a faint tint.
    double fill = std::max(0.0, -wi.y) * 0.05;
    return col + dvec3{fill * 0.7, fill * 0.8, fill};
}

struct BundleSpec {
    dvec3 T;
    dvec3 sigma;
    dvec3 center;
};

FiberScene build_scene() {
    FiberScene scene;
    FiberMaterial common;
    common.eta = 1.45;
    common.beta_m = 0.15;
    common.beta_n = 0.25;

    const BundleSpec specs[] = {
        {{0.0, 1.0, 0.0},  {0.20, 0.70, 1.50}, {-2.7, 0.0, -7.0}}, // warm
        {{0.6, 1.0, 0.0},  {1.50, 0.60, 0.25}, {-0.9, 0.0, -7.0}}, // cool
        {{0.9, 0.2, 0.4},  {0.80, 0.80, 0.80}, { 0.9, 0.0, -7.0}}, // gray
        {{-0.5, 1.0, 0.3}, {0.30, 1.20, 0.90}, { 2.7, 0.0, -7.0}}, // magenta
    };

    const double radius = 0.06;
    const double length = 3.4;
    for (const BundleSpec& spec : specs) {
        dvec3 T = spec.T.normalized();
        dvec3 ref = std::fabs(T.y) < 0.9 ? dvec3{0, 1, 0} : dvec3{1, 0, 0};
        dvec3 e1 = cross(T, ref).normalized();
        dvec3 e2 = cross(T, e1).normalized();
        // 5x5 filaments, spaced so the lateral surfaces never touch.
        for (int i = -2; i <= 2; ++i) {
            for (int j = -2; j <= 2; ++j) {
                dvec3 c = spec.center +
                          e1 * (i * 2.6 * radius) +
                          e2 * (j * 2.6 * radius);
                Fiber f;
                f.A = c - T * (0.5 * length);
                f.B = c + T * (0.5 * length);
                f.radius = radius;
                f.material = common;
                f.material.sigma_a = spec.sigma;
                scene.fibers.push_back(f);
            }
        }
    }

    // One original Sphere used purely as a black occluder at the lower left.
    Sphere blocker;
    blocker.center = {-3.4f, -1.7f, -7.0f};
    blocker.radius = 0.9f;
    scene.occluders.push_back(blocker);
    return scene;
}

dvec3 pixel_direction(int px, int py) {
    double dx = (px + 0.5) - WIDTH / 2.0;
    double dy = -(py + 0.5) + HEIGHT / 2.0;
    double dz = -HEIGHT / (2.0 * std::tan(FOV / 2.0));
    return dvec3{dx, dy, dz}.normalized();
}

char encode_channel(double linear) {
    // Fixed exposure plus a soft shoulder, then gamma 2.2.
    double x = std::max(0.0, linear) * 0.9;
    double mapped = x / (1.0 + x);
    double g = std::pow(mapped, 1.0 / 2.2);
    return char(std::lround(std::min(1.0, g) * 255.0));
}

// Nearest pixel whose primary ray aims at the given world point.
void pixel_towards(const dvec3& point, int& px, int& py) {
    const double K = HEIGHT / (2.0 * std::tan(FOV / 2.0));
    double dx = K * point.x / (-point.z);
    double dy = -K * point.y / (-point.z);
    px = int(std::lround(dx + WIDTH / 2.0 - 0.5));
    py = int(std::lround(-dy + HEIGHT / 2.0 - 0.5));
    px = std::max(0, std::min(WIDTH - 1, px));
    py = std::max(0, std::min(HEIGHT - 1, py));
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: " << argv[0] << " <output_directory>\n";
        return 2;
    }
    std::filesystem::path outdir = argv[1];
    std::filesystem::create_directories(outdir);

    FiberScene scene = build_scene();
    if (!fiber_scene_valid(scene)) {
        std::cerr << "invalid fiber scene\n";
        return 1;
    }

    std::vector<dvec3> frame(WIDTH * HEIGHT);
    for (int py = 0; py < HEIGHT; ++py) {
        for (int px = 0; px < WIDTH; ++px) {
            dvec3 d = pixel_direction(px, py);
            dvec3 acc{0, 0, 0};
            for (int s = 0; s < SPP; ++s) {
                // One fixed stream per (pixel, sample): fully deterministic.
                std::uint64_t seed =
                    0x9E3779B97F4A7C15ull +
                    (static_cast<std::uint64_t>(py * WIDTH + px) * SPP +
                     static_cast<std::uint64_t>(s)) *
                    0xD1B54A32D192ED03ull;
                DeterministicStream stream(seed);
                acc = acc + trace_fiber(scene, {0, 0, 0}, d, 3,
                                        stream, background_fn);
            }
            frame[py * WIDTH + px] = acc * (1.0 / SPP);
        }
    }

    std::filesystem::path ppm_path = outdir / "fiber_preview.ppm";
    std::ofstream ppm(ppm_path, std::ios::binary);
    ppm << "P6\n" << WIDTH << " " << HEIGHT << "\n255\n";
    for (const dvec3& c : frame) {
        ppm.put(encode_channel(c.x));
        ppm.put(encode_channel(c.y));
        ppm.put(encode_channel(c.z));
    }
    ppm.close();
    if (!ppm) {
        std::cerr << "failed to write " << ppm_path.string() << "\n";
        return 1;
    }

    // Representative rays: the image center and one ray aimed at each
    // bundle center, plus one near the black occluder.
    struct Rep { const char* name; dvec3 target; };
    const Rep reps[] = {
        {"center",       { 0.0,  0.0, -7.0}},
        {"warm_bundle",  {-2.7,  0.0, -7.0}},
        {"cool_bundle",  {-0.9,  0.0, -7.0}},
        {"gray_bundle",  { 0.9,  0.0, -7.0}},
        {"magenta_bundle",{2.7,  0.0, -7.0}},
        {"near_sphere",  {-3.4, -1.2, -7.0}},
    };
    std::filesystem::path rep_path = outdir / "fiber_preview_reps.txt";
    std::ofstream rep(rep_path);
    rep.setf(std::ios::scientific);
    rep.precision(10);
    for (const Rep& r : reps) {
        int px, py;
        pixel_towards(r.target, px, py);
        dvec3 d = pixel_direction(px, py);
        const dvec3& c = frame[py * WIDTH + px];
        rep << "ray=" << r.name
            << " pixel=" << px << "," << py
            << " dir=(" << d.x << "," << d.y << "," << d.z << ")"
            << " linear_rgb=(" << c.x << "," << c.y << "," << c.z << ")\n";
    }
    rep.close();
    if (!rep) {
        std::cerr << "failed to write " << rep_path.string() << "\n";
        return 1;
    }

    std::size_t lit = 0;
    for (const dvec3& c : frame)
        if (c.x + c.y + c.z > 0.05) ++lit;
    std::cout << "fiber_preview wrote " << ppm_path.string()
              << " and " << rep_path.string()
              << " lit_pixels=" << lit << "/" << frame.size() << "\n";
    return 0;
}
