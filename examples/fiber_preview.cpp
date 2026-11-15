// Fiber material preview.
//
// Usage: fiber_preview <output-directory>
//
// Builds a few bundles of thin open-cylinder fibers with different
// orientations and RGB absorption coefficients, one black sphere occluder, a
// directional background and a fixed per-pixel sample stream.  Writes a PPM
// image and a text record of the linear RGB carried by representative rays.
// C++17 standard library only.

#include "../fiber_renderer.h"
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {
constexpr int WIDTH = 320;
constexpr int HEIGHT = 240;
constexpr double FOV = 1.05;
constexpr int SPP = 8;
constexpr int MAX_BOUNCES = 4;

dvec3 nrm(dvec3 v) { return v.normalized(); }

Fiber make_thread(int id, dvec3 center, dvec3 tangent, double half_len,
                  double radius, double eta, dvec3 sigma_a,
                  double beta_m, double beta_n) {
    Fiber f;
    const dvec3 t = tangent.normalized();
    f.a = center - t*half_len;
    f.b = center + t*half_len;
    f.radius = radius;
    f.id = id;
    f.material.eta = eta;
    f.material.sigma_a = sigma_a;
    f.material.beta_m = beta_m;
    f.material.beta_n = beta_n;
    return f;
}

// Directional background: a neutral vertical gradient plus a tight white band
// around a fixed direction.  Keeping it achromatic makes the coloration picked
// up while crossing the colored fibers directly visible in the RGB ratios.
dvec3 background(const dvec3 &dir) {
    const dvec3 band_dir = nrm({-0.35, 0.55, -1.0});
    const double g = 0.5 + 0.5*dir.y;
    const double base = 0.16 + 0.10*g;
    dvec3 color{base, base, base};
    const double b = std::pow(std::max(0.0, dir*band_dir), 40.0);
    color = color + dvec3{1.6, 1.6, 1.6}*b;
    return color;
}

dvec3 pixel_direction(int x, int y) {
    const double dx = (x + 0.5) - WIDTH/2.0;
    const double dy = -(y + 0.5) + HEIGHT/2.0;
    const double dz = -HEIGHT/(2.0*std::tan(FOV/2.0));
    return nrm({dx, dy, dz});
}

char to_byte(double linear) {
    const double clamped = std::max(0.0, std::min(1.0, linear));
    return static_cast<char>(static_cast<unsigned>(clamped*255.0 + 0.5));
}
} // namespace

int main(int argc, char **argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: %s <output-directory>\n", argv[0]);
        return 1;
    }
    const std::filesystem::path out_dir = argv[1];
    std::error_code ec;
    std::filesystem::create_directories(out_dir, ec);
    if (ec) {
        std::fprintf(stderr, "cannot create output directory: %s\n", ec.message().c_str());
        return 1;
    }

    std::vector<Fiber> fibers;
    int next_id = 0;

    // Bundle A: horizontal threads along x, nearly neutral (low absorption),
    // which read as bright bands stretched along the thread direction.
    const dvec3 t_x{1, 0, 0};
    for (int i = 0; i < 7; ++i) {
        const double y = -2.1 + i*0.7;
        fibers.push_back(make_thread(next_id++, {0.0, y, -9.0}, t_x, 5.6, 0.045,
                                     1.45, {0.05, 0.05, 0.06}, 0.30, 0.55));
    }

    // Bundle B: tilted threads, strongly absorbing green/blue -> warm red.
    const dvec3 t_b = nrm({0.25, 1.0, 0.18});
    for (int i = 0; i < 6; ++i) {
        const double x = -3.6 + i*1.35;
        fibers.push_back(make_thread(next_id++, {x, 0.0, -10.6}, t_b, 3.4, 0.05,
                                     1.5, {0.15, 1.7, 2.6}, 0.25, 0.45));
    }

    // Bundle C: diagonal threads, strongly absorbing red/green -> cool blue.
    const dvec3 t_c = nrm({1.0, 0.6, 0.28});
    for (int i = 0; i < 6; ++i) {
        const double y = -2.4 + i*0.95;
        fibers.push_back(make_thread(next_id++, {0.5, y, -12.2}, t_c, 4.2, 0.055,
                                     1.55, {2.4, 1.2, 0.15}, 0.35, 0.6));
    }

    // One black sphere occluder from the original core.
    Sphere occluder;
    occluder.center = {3.1f, 1.3f, -9.6f};
    occluder.radius = 0.85f;

    FiberScene scene;
    scene.fibers = fibers.data();
    scene.fiber_count = fibers.size();
    scene.spheres = &occluder;
    scene.sphere_count = 1;

    std::vector<dvec3> framebuffer(WIDTH*HEIGHT);
    // Per-pixel first-hit classification: >= 0 fiber id, -1 black sphere,
    // -2 escaped to the background.
    std::vector<int> first_hit(WIDTH*HEIGHT, -2);
    std::uint64_t event_pixels = 0;
    const vec3 camera32{0.0f, 0.0f, 0.0f};
    for (int y = 0; y < HEIGHT; ++y) {
        for (int x = 0; x < WIDTH; ++x) {
            const dvec3 dir = pixel_direction(x, y);
            dvec3 accum{0, 0, 0};
            for (int s = 0; s < SPP; ++s) {
                // Fixed stream: the seed is a pure function of pixel and sample.
                SampleStream stream(0x51eedULL ^ (std::uint64_t(y*WIDTH + x) << 8) ^
                                    std::uint64_t(s));
                accum = accum + trace_fiber_ray(scene, {0, 0, 0}, dir,
                                                MAX_BOUNCES, stream, background);
            }
            framebuffer[y*WIDTH + x] = accum*(1.0/SPP);

            const FiberHit fh = intersect_fibers(fibers.data(), fibers.size(),
                                                 {0,0,0}, dir, 0.001, 1.0e10, -1);
            const SphereHit sh = ray_sphere_intersect(camera32, dir.to_vec3(), occluder);
            int kind = -2;
            if (fh.id >= 0 && (!sh.hit || fh.t < sh.t)) { kind = fh.id; ++event_pixels; }
            else if (sh.hit) kind = -1;                  // exact ties go to the sphere
            first_hit[y*WIDTH + x] = kind;
        }
    }

    // PPM output (clamped linear RGB bytes).
    const std::filesystem::path ppm_path = out_dir / "fiber_preview.ppm";
    std::ofstream ppm(ppm_path, std::ios::binary);
    if (!ppm) { std::fprintf(stderr, "cannot write %s\n", ppm_path.string().c_str()); return 1; }
    ppm << "P6\n" << WIDTH << " " << HEIGHT << "\n255\n";
    for (const dvec3 &c : framebuffer) {
        for (int k = 0; k < 3; ++k)
            ppm.put(to_byte(c[k]));
    }
    ppm.close();

    // Representative rays: pick real pixels from the first-hit table, one per
    // bundle plus the sphere and a background pixel, and record their exact
    // linear RGB and sample consumption.
    struct Probe { int id; const char *label; };
    const Probe probes[] = {
        {3,  "horizontal_bundle"},
        {9,  "red_bundle"},
        {15, "blue_bundle"},
        {-1, "sphere_occluder"},
        {-2, "background"},
    };
    const std::filesystem::path txt_path = out_dir / "fiber_representative.txt";
    std::ofstream txt(txt_path);
    if (!txt) { std::fprintf(stderr, "cannot write %s\n", txt_path.string().c_str()); return 1; }
    txt.precision(8);
    txt << "# representative linear RGB records (one sample per ray)\n";
    txt << "# fibers=" << fibers.size() << " spp=" << SPP
        << " max_fiber_bounces=" << MAX_BOUNCES << "\n";
    for (const Probe &p : probes) {
        int px = -1, py = -1;
        for (int idx = 0; idx < WIDTH*HEIGHT; ++idx) {
            if (first_hit[idx] == p.id) { px = idx % WIDTH; py = idx / WIDTH; break; }
        }
        if (px < 0) { std::fprintf(stderr, "no pixel found for %s\n", p.label); return 1; }
        const dvec3 dir = pixel_direction(px, py);
        SampleStream stream(0xA5A5A5A5ULL ^ std::uint64_t(py*WIDTH + px));
        const dvec3 rgb = trace_fiber_ray(scene, {0,0,0}, dir,
                                          MAX_BOUNCES, stream, background);
        txt << p.label << " pixel=(" << px << "," << py << ")"
            << " first_hit=" << first_hit[py*WIDTH + px]
            << " samples_consumed=" << stream.consumed()
            << " dir=(" << dir.x << "," << dir.y << "," << dir.z << ")"
            << " linear_rgb=(" << rgb.x << "," << rgb.y << "," << rgb.z << ")\n";
    }
    txt << "pixels_meeting_fibers=" << event_pixels << "\n";
    txt.close();

    std::printf("wrote %s and %s, fiber pixels=%llu\n",
                ppm_path.string().c_str(), txt_path.string().c_str(),
                static_cast<unsigned long long>(event_pixels));
    return 0;
}
