// Windowless projective texturing demo.
//
// It generates, purely from C++:
//   * a seam grid: two world-adjacent panels whose UVs live in separate
//     islands, plus a small foreground occluder quad in its own UV island;
//   * two source views with different solid observation colors and cameras
//     looking from opposite sides;
//   * the baked floating point atlas, exported as TGA (color and weight);
//   * a Phong rendering of the model with the baked atlas installed as its
//     diffuse material, going through the original IShader rasterize path.
//
// Usage: projective_baker <output_directory>

#include "projective_baker.h"
#include "phong_shader.h"
#include "our_gl.h"
#include "tgaimage.h"
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

using namespace projective_baker;

namespace {

constexpr int kSourceW = 32;
constexpr int kSourceH = 32;
constexpr int kAtlasW = 64;
constexpr int kAtlasH = 64;

struct ObjBuilder {
    std::vector<vec3> verts;
    std::vector<vec2> uvs; // already-flipped Model-space UVs (v grows downward)
    struct Face { int v[3], t[3]; };
    std::vector<Face> faces;

    int add_vert(const vec3 &p) { verts.push_back(p); return static_cast<int>(verts.size()) - 1; }
    int add_uv(const vec2 &t) { uvs.push_back(t); return static_cast<int>(uvs.size()) - 1; }
    void add_face(int a, int b, int c, int ta, int tb, int tc) {
        faces.push_back({{a, b, c}, {ta, tb, tc}});
    }

    void write(const std::string &path) const {
        std::ofstream out(path);
        for (const vec3 &p : verts)
            out << "v " << p.x << ' ' << p.y << ' ' << p.z << '\n';
        for (const vec2 &t : uvs)
            out << "vt " << t.x << ' ' << (1. - t.y) << '\n'; // Model flips vt y again
        out << "vn 0 0 1\n";
        for (const Face &f : faces)
            out << "f " << f.v[0] + 1 << '/' << f.t[0] + 1 << "/1 "
                << f.v[1] + 1 << '/' << f.t[1] + 1 << "/1 "
                << f.v[2] + 1 << '/' << f.t[2] + 1 << "/1\n";
    }
};

// A subdivided panel: grid_x*grid_y quads, each mapped to its own sub-rect
// of a UV island (duplicated UV corners create explicit seams at island bounds).
void add_panel(ObjBuilder &obj, const double x0, const double x1,
               const double y0, const double y1, const double z,
               const double u0, const double u1,
               const double v0, const double v1,
               const int grid_x, const int grid_y) {
    std::vector<std::vector<int>> v(grid_y + 1, std::vector<int>(grid_x + 1));
    for (int j = 0; j <= grid_y; ++j)
        for (int i = 0; i <= grid_x; ++i) {
            double x = x0 + (x1 - x0) * i / grid_x;
            double y = y0 + (y1 - y0) * j / grid_y;
            v[j][i] = obj.add_vert({x, y, z});
        }
    for (int j = 0; j < grid_y; ++j) {
        for (int i = 0; i < grid_x; ++i) {
            double fu0 = u0 + (u1 - u0) * i / grid_x;
            double fu1 = u0 + (u1 - u0) * (i + 1) / grid_x;
            double fv0 = v0 + (v1 - v0) * j / grid_y;
            double fv1 = v0 + (v1 - v0) * (j + 1) / grid_y;
            int t00 = obj.add_uv({fu0, fv0}), t10 = obj.add_uv({fu1, fv0});
            int t11 = obj.add_uv({fu1, fv1}), t01 = obj.add_uv({fu0, fv1});
            int a = v[j][i], b = v[j][i + 1], c = v[j + 1][i + 1], d = v[j + 1][i];
            obj.add_face(a, b, c, t00, t10, t11);
            obj.add_face(a, c, d, t00, t11, t01);
        }
    }
}

std::vector<double> solid_image(const double r, const double g, const double b) {
    std::vector<double> img(static_cast<size_t>(kSourceW) * kSourceH * 3u);
    for (size_t p = 0; p < static_cast<size_t>(kSourceW) * kSourceH; ++p) {
        img[p * 3] = r; img[p * 3 + 1] = g; img[p * 3 + 2] = b;
    }
    return img;
}

std::uint8_t to_byte(const double x) {
    return static_cast<std::uint8_t>(std::clamp(x, 0., 1.) * 255. + .5);
}

void write_rgb_tga(const std::string &path, const std::vector<double> &rgb,
                   const int w, const int h) {
    TGAImage img(w, h, TGAImage::RGB);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            size_t p = (static_cast<size_t>(y) * w + x) * 3u;
            img.set(x, y, {{to_byte(rgb[p + 2]), to_byte(rgb[p + 1]),
                            to_byte(rgb[p]), 255}, 3}); // BGRA
        }
    img.write_tga_file(path);
}

void write_weight_tga(const std::string &path, const BakeResult &bake) {
    TGAImage img(bake.width, bake.height, TGAImage::GRAYSCALE);
    double max_weight = 0.;
    for (double w : bake.weight) max_weight = std::max(max_weight, w);
    for (int y = 0; y < bake.height; ++y)
        for (int x = 0; x < bake.width; ++x) {
            size_t i = static_cast<size_t>(y) * bake.width + x;
            double n = max_weight > 0. ? bake.weight[i] / max_weight : 0.;
            img.set(x, y, {{to_byte(n)}, 1});
        }
    img.write_tga_file(path);
}

mat<4, 4> camera_matrix(const vec3 &eye, const vec3 &center, const vec3 &up) {
    lookat(eye, center, up);
    init_perspective(norm(eye - center));
    return Perspective * ModelView; // world-to-clip
}

} // namespace

int main(int argc, char **argv) {
    if (argc != 2) {
        std::cerr << "Usage: " << argv[0] << " <output_directory>\n";
        return 1;
    }
    const std::filesystem::path out = argv[1];
    std::filesystem::create_directories(out);

    // 1. Seam grid: left and right panels share a world edge but own separate
    //    UV islands; the foreground quad is an occluder in a third island.
    ObjBuilder obj;
    add_panel(obj, -.5, 0., -.5, .5, 0., .05, .45, .1, .9, 2, 2);  // left panel
    add_panel(obj, 0., .5, -.5, .5, 0., .55, .95, .1, .9, 2, 2);   // right panel
    add_panel(obj, -.35, -.2, -.15, -.05, .4, .02, .12, .02, .08, 1, 1); // occluder
    const std::string obj_path = (out / "scene.obj").string();
    obj.write(obj_path);

    Model model(obj_path);
    ProjectiveGeometry geometry({&model});

    // 2. Two observation views with distinct colors and opposite-side cameras.
    const std::vector<double> image_a = solid_image(.85, .12, .06); // warm red
    const std::vector<double> image_b = solid_image(.06, .25, .85); // cool blue
    mat<4, 4> clip_a = camera_matrix({-.9, -.2, 1.6}, {0., 0., 0.}, {0., 1., 0.});
    mat<4, 4> clip_b = camera_matrix({ .9, -.2, 1.6}, {0., 0., 0.}, {0., 1., 0.});
    SourceCapture view_a(geometry, clip_a, {-.9, -.2, 1.6}, image_a, kSourceW, kSourceH);
    SourceCapture view_b(geometry, clip_b, { .9, -.2, 1.6}, image_b, kSourceW, kSourceH);

    write_rgb_tga((out / "source_a.tga").string(), image_a, kSourceW, kSourceH);
    write_rgb_tga((out / "source_b.tga").string(), image_b, kSourceW, kSourceH);

    // 3. Projective bake into a floating point corner-UV atlas.
    std::vector<OverlapConflict> conflicts;
    BakeResult bake = bake_atlas(geometry, {&view_a, &view_b},
                                 kAtlasW, kAtlasH, 1e-9, &conflicts);
    if (!conflicts.empty()) {
        std::cerr << "unexpected UV overlap conflicts: " << conflicts.size() << '\n';
        return 2;
    }
    write_rgb_tga((out / "baked_atlas.tga").string(), bake.rgb, kAtlasW, kAtlasH);
    write_weight_tga((out / "baked_weight.tga").string(), bake);

    // 4. Install the baked atlas as the model's diffuse material (plus neutral
    //    normal/specular maps) and render through the original Phong path.
    TGAImage diffuse(kAtlasW, kAtlasH, TGAImage::RGB);
    for (int y = 0; y < kAtlasH; ++y)
        for (int x = 0; x < kAtlasW; ++x) {
            size_t p = (static_cast<size_t>(y) * kAtlasW + x) * 3u;
            diffuse.set(x, y, {{to_byte(bake.rgb[p + 2]), to_byte(bake.rgb[p + 1]),
                                to_byte(bake.rgb[p]), 255}, 3});
        }
    TGAImage normal_map(kAtlasW, kAtlasH, TGAImage::RGB, {{255, 128, 128, 255}, 3});
    TGAImage specular_map(kAtlasW, kAtlasH, TGAImage::GRAYSCALE, {{0}, 1});
    const std::string base = (out / "scene").string();
    if (!diffuse.write_tga_file(base + "_diffuse.tga") ||
        !normal_map.write_tga_file(base + "_nm_tangent.tga") ||
        !specular_map.write_tga_file(base + "_spec.tga"))
        return 3;

    Model textured(base + ".obj");
    constexpr int width = 400, height = 400;
    lookat({-1., .3, 1.8}, {0., 0., 0.}, {0., 1., 0.});
    init_perspective(norm(vec3{-1., .3, 1.8}));
    init_viewport(width / 8, height / 8, width * 3 / 4, height * 3 / 4);
    init_zbuffer(width, height);
    TGAImage framebuffer(width, height, TGAImage::RGB, {{177, 195, 209, 255}, 3});
    PhongShader shader({1., 1., 1.}, textured);
    for (int f = 0; f < textured.nfaces(); ++f) {
        Triangle clip = {shader.vertex(f, 0), shader.vertex(f, 1), shader.vertex(f, 2)};
        rasterize(clip, shader, framebuffer);
    }
    if (!framebuffer.write_tga_file((out / "framebuffer.tga").string()))
        return 4;

    std::cout << "projective bake done: "
              << "covered texels=" << std::count(bake.covered.begin(), bake.covered.end(), 1)
              << ", sources=2 (red/blue), occluder quad present\n"
              << "outputs in " << out << ": baked_atlas.tga, baked_weight.tga, "
                 "source_a.tga, source_b.tga, framebuffer.tga, scene.obj\n";
    return 0;
}
