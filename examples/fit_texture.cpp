// Texture fitting demo for the differentiable rasterizer.
//
// Generates a small triangulated OBJ and a deliberately uneven TGA diffuse
// map, renders it through Model -> make_scene -> diff_forward, then takes a
// few gradient-descent steps on the texture using diff_backward. The error is
// measured on the pixels visible in the initial frame and reported before and
// after optimization; two TGA images are written for comparison.
//
// Usage: fit_texture <output-directory>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

#include "differentiable.h"
#include "model.h"
#include "our_gl.h"
#include "tgaimage.h"

namespace {

constexpr int kWidth  = 96;
constexpr int kHeight = 64;
constexpr int kTexW   = 8;
constexpr int kTexH   = 6;
constexpr vec3 kLight = {0.35, 0.25, 0.90}; // unit length, view space

std::string join(const std::string &dir, const std::string &name) {
    if (dir.empty()) return name;
    return dir.back() == '/' ? dir + name : dir + "/" + name;
}

// Write the generated OBJ. UVs are written as Model expects them (it flips v).
void write_obj(const std::string &path) {
    std::ofstream o(path);
    o << "v -0.9 -0.7 -0.4\n"
         "v  0.9 -0.7  0.4\n"
         "v  0.9  0.7 -0.4\n"
         "v -0.9  0.7  0.4\n";
    o << "vt 0.05 0.05\nvt 0.95 0.05\nvt 0.95 0.95\nvt 0.05 0.95\n";
    o << "vn 0 0 1\n";
    o << "f 1/1/1 2/2/1 3/3/1\n"
         "f 1/1/1 3/3/1 4/4/1\n";
}

// A diffuse map with clear per-texel structure (written via TGAImage, which
// stores BGR; make_scene must read it back as RGB).
void write_texture(const std::string &path) {
    TGAImage img(kTexW, kTexH, TGAImage::RGB, TGAColor{0,0,0,255});
    for (int y=0; y<kTexH; ++y)
        for (int x=0; x<kTexW; ++x) {
            const int r = (x*37 + y*11 + 20) % 256;
            const int g = (x*7  + y*53 + 60) % 256;
            const int b = (x*29 + y*17 + 90) % 256;
            img.set(x, y, TGAColor{static_cast<std::uint8_t>(b),
                                   static_cast<std::uint8_t>(g),
                                   static_cast<std::uint8_t>(r), 255});
        }
    img.write_tga_file(path, false, false);
}

void write_rgb_image(const std::string &path, const std::vector<double> &rgb,
                     const int w, const int h) {
    TGAImage img(w, h, TGAImage::RGB, TGAColor{0,0,0,255});
    for (int y=0; y<h; ++y)
        for (int x=0; x<w; ++x) {
            const int i = 3*(x + y*w);
            auto to8 = [](const double v) {
                const double q = std::min(1.0, std::max(0.0, v));
                return static_cast<std::uint8_t>(std::lround(q*255.0));
            };
            img.set(x, y, TGAColor{to8(rgb[i+2]), to8(rgb[i+1]), to8(rgb[i+0]), 255});
        }
    img.write_tga_file(path, false, false);
}

void write_texture_image(const std::string &path, const DiffScene &s) {
    TGAImage img(s.tex_width, s.tex_height, TGAImage::RGB, TGAColor{0,0,0,255});
    for (int y=0; y<s.tex_height; ++y)
        for (int x=0; x<s.tex_width; ++x) {
            const int i = 3*(x + y*s.tex_width);
            auto to8 = [](const double v) {
                const double q = std::min(1.0, std::max(0.0, v));
                return static_cast<std::uint8_t>(std::lround(q*255.0));
            };
            img.set(x, y, TGAColor{to8(s.tex_rgb[i+2]), to8(s.tex_rgb[i+1]),
                                   to8(s.tex_rgb[i+0]), 255});
        }
    img.write_tga_file(path, false, false);
}

// Mean squared error over the pixels visible in the reference frame.
double visible_mse(const DiffForward &frame, const std::vector<double> &target,
                   const std::vector<int> &pixels) {
    double sum = 0.0;
    int n = 0;
    for (const int pi : pixels) {
        for (int c=0; c<3; ++c) {
            const double d = frame.rgb[3*pi+c] - target[3*pi+c];
            sum += d*d;
        }
        ++n;
    }
    return n ? sum / (3.0*n) : 0.0;
}

std::vector<int> visible_pixels(const DiffForward &frame) {
    std::vector<int> pixels;
    for (int y=0; y<frame.height; ++y)
        for (int x=0; x<frame.width; ++x) {
            const int pi = x + y*frame.width;
            if (frame.face_id[pi] >= 0) pixels.push_back(pi);
        }
    return pixels;
}

} // namespace

int main(int argc, char **argv) {
    if (argc != 2) {
        std::fprintf(stderr, "Usage: %s <output-directory>\n", argv[0]);
        return 1;
    }
    const std::string out = argv[1];

    // Build the inputs on disk, then go through Model exactly as the classic
    // renderer does.
    const std::string obj_path = join(out, "fit_texture.obj");
    const std::string tex_path = join(out, "fit_texture_diffuse.tga");
    write_obj(obj_path);
    write_texture(tex_path);

    Model model(obj_path);
    if (model.nfaces() == 0) {
        std::fprintf(stderr, "failed to load %s\n", obj_path.c_str());
        return 1;
    }
    DiffScene scene = make_scene(model);

    // Target: the same scene with a different (smooth) texture.
    DiffScene target_scene = scene;
    for (int y=0; y<target_scene.tex_height; ++y)
        for (int x=0; x<target_scene.tex_width; ++x) {
            const int i = 3*(x + y*target_scene.tex_width);
            target_scene.tex_rgb[i+0] = 0.20 + 0.70*x/(target_scene.tex_width-1);
            target_scene.tex_rgb[i+1] = 0.75 - 0.55*y/(target_scene.tex_height-1);
            target_scene.tex_rgb[i+2] = 0.50;
        }
    write_texture_image(join(out, "fit_texture_target.tga"), target_scene);

    // Camera state, exactly as the legacy renderer configures it.
    const vec3 eye{-0.6, 0.5, 2.6};
    const vec3 center{0.0, 0.0, 0.0};
    const vec3 up{0.0, 1.0, 0.0};
    lookat(eye, center, up);
    init_perspective(norm(eye-center));
    init_viewport(0, 0, kWidth, kHeight);

    const DiffForward initial = diff_forward(scene, kWidth, kHeight, kLight);
    const std::vector<int> pixels = visible_pixels(initial);
    if (pixels.empty()) {
        std::fprintf(stderr, "no visible pixels\n");
        return 1;
    }
    const std::vector<double> target = [&] {
        const DiffForward t = diff_forward(target_scene, kWidth, kHeight, kLight);
        // Take the target colour on the same visible set; pixels hidden in the
        // initial frame are not part of the loss.
        return t.rgb;
    }();
    write_rgb_image(join(out, "fit_texture_before.tga"), initial.rgb, kWidth, kHeight);
    const double error_before = visible_mse(initial, target, pixels);

    // Gradient descent on the texture only. Positions and UVs stay fixed.
    // The step is scaled by the gradient norm so it stays stable regardless of
    // how many pixels happen to be visible.
    constexpr int kSteps = 40;
    constexpr double kRate = 0.35;
    DiffForward frame = initial;
    for (int step=0; step<kSteps; ++step) {
        // d(0.5*||C-target||^2)/dC over the fixed visible set.
        std::vector<double> g(3*kWidth*kHeight, 0.0);
        for (const int pi : pixels)
            for (int c=0; c<3; ++c)
                g[3*pi+c] = frame.rgb[3*pi+c] - target[3*pi+c];
        const DiffGrad grad = diff_backward(frame, g);
        double norm2 = 0.0;
        for (const double v : grad.tex_rgb) norm2 += v*v;
        const double scale = std::sqrt(norm2) > 1e-12 ? kRate/std::sqrt(norm2) : 0.0;
        for (size_t i=0; i<scene.tex_rgb.size(); ++i)
            scene.tex_rgb[i] = std::min(1.0, std::max(0.0, scene.tex_rgb[i] - scale*grad.tex_rgb[i]));
        frame = diff_forward(scene, kWidth, kHeight, kLight);
    }
    const double error_after = visible_mse(frame, target, pixels);

    write_rgb_image(join(out, "fit_texture_after.tga"), frame.rgb, kWidth, kHeight);
    write_texture_image(join(out, "fit_texture_fitted.tga"), scene);

    std::printf("visible_pixels=%zu texture=%dx%d steps=%d\n",
                pixels.size(), scene.tex_width, scene.tex_height, kSteps);
    std::printf("mse_before=%.9f mse_after=%.9f\n", error_before, error_after);
    if (!(error_after < error_before)) {
        std::fprintf(stderr, "optimization did not reduce the visible error\n");
        return 1;
    }
    return 0;
}
