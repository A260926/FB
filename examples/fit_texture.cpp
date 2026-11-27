// Texture-fitting example for the differentiable renderer.
//
// It generates a small OBJ mesh and a non-uniform TGA texture in C++, loads
// them through Model, builds an owned DiffScene, and fits the floating-point
// RGB texture to a fixed target image using only the hand-written adjoint
// (gradient descent, no autodiff library). Visibility is fixed from the first
// render; the squared error is tracked on exactly those pixels, and
// before/after TGA images are written to the output directory.
//
// Usage: fit_texture <output-directory>
#include "differentiable.h"
#include "our_gl.h"
#include "model.h"
#include "tgaimage.h"

#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

constexpr int WIDTH = 32;
constexpr int HEIGHT = 32;
constexpr int TEX_W = 4;
constexpr int TEX_H = 3;
constexpr int STEPS = 80;
constexpr double LEARNING_RATE = 1.0;

void write_obj(const fs::path &file) {
    std::ofstream out(file);
    out << "v -1.3 -1.3 0.0\n"
           "v  1.3 -1.3 0.2\n"
           "v  1.3  1.3 -0.1\n"
           "v -1.3  1.3 0.1\n"
           "vt 0.02 0.02\n"
           "vt 0.98 0.05\n"
           "vt 0.95 0.95\n"
           "vt 0.05 0.92\n"
           "vn 0 0 1\n"
           "f 1/1/1 2/2/1 3/3/1\n"
           "f 1/1/1 3/3/1 4/4/1\n";
}

// Non-uniform initial guess: deliberately far from the smooth target.
void write_initial_texture(const fs::path &file) {
    TGAImage img(TEX_W, TEX_H, TGAImage::RGB);
    for (int y = 0; y < TEX_H; ++y)
        for (int x = 0; x < TEX_W; ++x) {
            const std::uint8_t r = static_cast<std::uint8_t>(30 + 47 * x + 13 * y);
            const std::uint8_t g = static_cast<std::uint8_t>(200 - 31 * x + 17 * y);
            const std::uint8_t b = static_cast<std::uint8_t>(40 + 23 * ((x + y) % 3));
            img.set(x, y, TGAColor{{b, g, r, 255}}); // TGA storage order is BGRA
        }
    img.write_tga_file(file.string(), /*vflip=*/false);
}

// Procedural target texture (linear RGB).
double target_texel(const int x, const int y, const int channel) {
    const double u = static_cast<double>(x) / (TEX_W - 1);
    const double v = static_cast<double>(y) / (TEX_H - 1);
    if (channel == 0) return 0.15 + 0.65 * u;
    if (channel == 1) return 0.85 - 0.55 * v;
    return 0.25 + 0.45 * u * v;
}

void save_linear_image(const fs::path &file, const DiffImage &image) {
    TGAImage out(WIDTH, HEIGHT, TGAImage::RGB);
    for (int y = 0; y < HEIGHT; ++y)
        for (int x = 0; x < WIDTH; ++x) {
            const int pi = x + y * WIDTH;
            auto to_byte = [](double v) {
                v = std::min(1.0, std::max(0.0, v));
                return static_cast<std::uint8_t>(std::lround(v * 255.0));
            };
            const std::uint8_t r = to_byte(image.rgb[pi*3+0]);
            const std::uint8_t g = to_byte(image.rgb[pi*3+1]);
            const std::uint8_t b = to_byte(image.rgb[pi*3+2]);
            out.set(x, y, TGAColor{{b, g, r, 255}});
        }
    out.write_tga_file(file.string(), /*vflip=*/false);
}

double squared_error(const DiffImage &image, const DiffImage &target,
                     const std::vector<int> &visible) {
    double e = 0;
    for (const int pi : visible)
        for (int c = 0; c < 3; ++c) {
            const double d = image.rgb[pi*3+c] - target.rgb[pi*3+c];
            e += d * d;
        }
    return e;
}

} // namespace

int main(int argc, char **argv) {
    if (argc != 2) {
        std::cerr << "Usage: " << argv[0] << " <output-directory>\n";
        return 1;
    }
    const fs::path out_dir = argv[1];
    fs::create_directories(out_dir);
    const fs::path obj_file = out_dir / "fit_scene.obj";
    const fs::path tex_file = out_dir / "fit_scene_diffuse.tga";
    write_obj(obj_file);
    write_initial_texture(tex_file);

    lookat({0, 0, 3.2}, {0, 0, 0}, {0, 1, 0});
    init_perspective(3.2);
    init_viewport(0, 0, WIDTH, HEIGHT);
    init_zbuffer(WIDTH, HEIGHT);

    Model model(obj_file.string());
    auto scene = DiffScene::from_model(model, WIDTH, HEIGHT);
    const vec3 light{0, 0, 1};

    // Fixed visible set from the first render; texture edits never change
    // coverage because geometry and the camera are untouched.
    DiffImage before;
    auto record = diff_render(*scene, light, before);
    std::vector<int> visible;
    visible.reserve(record->pixels.size());
    for (const auto &p : record->pixels)
        visible.push_back(p.x + p.y * WIDTH);

    // Build the target image by loading the procedural target into the scene
    // texture, rendering once, then restoring the TGA-derived initial guess.
    std::vector<double> initial_texture = scene->texture;
    for (int y = 0; y < TEX_H; ++y)
        for (int x = 0; x < TEX_W; ++x)
            for (int c = 0; c < 3; ++c)
                scene->texel(x, y, c) = target_texel(x, y, c);
    DiffImage target;
    auto target_record = diff_render(*scene, light, target);
    (void)target_record;
    scene->texture = initial_texture;

    const double initial_error = squared_error(before, target, visible);

    // Gradient descent on the texture. Per-pixel adjoint of the mean squared
    // error is (render - target)/N; visibility/faces stay frozen across steps.
    const double inv_n = 1.0 / static_cast<double>(visible.size());
    for (int step = 0; step < STEPS; ++step) {
        DiffImage image;
        auto step_record = diff_render(*scene, light, image);
        std::vector<double> g(WIDTH * HEIGHT * 3, 0.0);
        for (const int pi : visible)
            for (int c = 0; c < 3; ++c)
                g[pi*3+c] = (image.rgb[pi*3+c] - target.rgb[pi*3+c]) * inv_n;
        const DiffGradients grad = diff_backward(*step_record, g);
        for (size_t i = 0; i < scene->texture.size(); ++i)
            scene->texture[i] -= LEARNING_RATE * grad.texture[i];
    }

    DiffImage after;
    auto final_record = diff_render(*scene, light, after);
    (void)final_record;
    const double final_error = squared_error(after, target, visible);

    save_linear_image(out_dir / "fit_before.tga", before);
    save_linear_image(out_dir / "fit_after.tga", after);
    save_linear_image(out_dir / "fit_target.tga", target);

    std::cout << "visible_pixels=" << visible.size()
              << " initial_sse=" << initial_error
              << " final_sse=" << final_error << '\n';
    if (!(final_error < initial_error)) {
        std::cerr << "fit failed: error did not decrease\n";
        return 2;
    }
    return 0;
}
