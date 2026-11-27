// Differentiable renderer cases: forward, hand-written adjoint vs central
// finite differences (the numerical reference lives only in this file), and
// forward-record lifetime/independence. No main(): task_support/driver.c runs
// the registered cases.
#include "differentiable.h"
#include "our_gl.h"
#include "model.h"
#include "tgaimage.h"
#include "task_support/check.h"

#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <unistd.h>
#include <vector>

namespace {

namespace fs = std::filesystem;

constexpr int IMG_W = 24;
constexpr int IMG_H = 24;

fs::path work_dir() {
    static fs::path dir;
    if (dir.empty()) {
        dir = fs::temp_directory_path() / ("diff_render_tests_" + std::to_string(::getpid()));
        fs::create_directories(dir);
    }
    return dir;
}
fs::path obj_path(const fs::path &stem) { fs::path p = stem; p += ".obj"; return p; }
fs::path tex_path(const fs::path &stem) { fs::path p = stem; p += "_diffuse.tga"; return p; }

void write_obj(const fs::path &file, const std::vector<vec4> &verts,
               const std::vector<vec2> &uvs, const std::vector<std::array<int,6>> &faces) {
    std::ofstream out(file);
    for (const auto &v : verts) out << "v " << v.x << ' ' << v.y << ' ' << v.z << '\n';
    for (const auto &t : uvs)   out << "vt " << t.x << ' ' << t.y << '\n';
    out << "vn 0 0 1\n";
    for (const auto &f : faces)
        out << "f " << f[0] << '/' << f[1] << "/1 "
                   << f[2] << '/' << f[3] << "/1 "
                   << f[4] << '/' << f[5] << "/1\n";
}

bool write_texture(const fs::path &file, const int w, const int h,
                   const std::function<void(int,int,std::uint8_t[3])> &color) {
    TGAImage img(w, h, TGAImage::RGB);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            std::uint8_t rgb[3];
            color(x, y, rgb);
            img.set(x, y, TGAColor{{rgb[2], rgb[1], rgb[0], 255}}); // TGA stores BGRA
        }
    return img.write_tga_file(file.string(), /*vflip=*/false);
}
#define WRITE_TEX(...) TASK_CHECK(write_texture(__VA_ARGS__))

void setup_camera() {
    lookat({0, 0, 3}, {0, 0, 0}, {0, 1, 0});
    init_perspective(3.0);
    init_viewport(0, 0, IMG_W, IMG_H);
    init_zbuffer(IMG_W, IMG_H);
}

bool close_enough(const double a, const double b) {
    return std::fabs(a - b) <= 2e-5 + 2e-5 * std::fabs(b);
}

// Scalar test loss: sum of the three linear-RGB channels at one pixel.
double pixel_loss(const DiffScene &scene, const vec3 &light, const int pixel) {
    DiffImage image;
    auto record = diff_render(scene, light, image);
    (void)record;
    return image.rgb[pixel*3+0] + image.rgb[pixel*3+1] + image.rgb[pixel*3+2];
}

std::vector<double> loss_adjoint(const DiffRecord &record, const int pixel) {
    std::vector<double> g(record.width * record.height * 3, 0.0);
    g[pixel*3+0] = g[pixel*3+1] = g[pixel*3+2] = 1.0;
    return g;
}

const DiffPixelRecord* find_pixel_record(const DiffRecord &record, const int pixel) {
    for (const auto &p : record.pixels)
        if (p.x + p.y * record.width == pixel) return &p;
    return nullptr;
}

} // namespace

// ---------------------------------------------------------------------------
// forward group
// ---------------------------------------------------------------------------

static int forward_perspective_and_rgb_order() {
    const fs::path stem = work_dir() / "fwd";
    // One tilted triangle: clip-space w differs between the corners.
    write_obj(obj_path(stem),
              {{-1.2,-1.2,-0.4,1}, {1.2,-1.2,0.3,1}, {0,1.2,0.1,1}},
              {{0.05,0.05}, {0.95,0.1}, {0.5,0.9}},
              {{1,1, 2,2, 3,3}});
    static const std::uint8_t table[2][2][3] = {
        {{10,20,30},   {40,50,60}},
        {{70,80,90},   {100,110,120}}};
    WRITE_TEX(tex_path(stem), 2, 2,
                  [](int x, int y, std::uint8_t rgb[3]) {
                      // Every texel has a distinct channel-dominant color so an
                      // R/B swap or wrong blend cannot pass.
                      rgb[0]=table[y][x][0]; rgb[1]=table[y][x][1]; rgb[2]=table[y][x][2];
                  });
    setup_camera();
    Model model(obj_path(stem).string());
    auto scene = DiffScene::from_model(model, IMG_W, IMG_H);

    // Scene texture must be linear RGB, not the TGA's BGR storage order.
    TASK_CHECK(std::fabs(scene->texel(0, 0, 0) - 10.0/255.0) < 1e-12);
    TASK_CHECK(std::fabs(scene->texel(0, 0, 1) - 20.0/255.0) < 1e-12);
    TASK_CHECK(std::fabs(scene->texel(0, 0, 2) - 30.0/255.0) < 1e-12);

    DiffImage image;
    auto record = diff_render(*scene, vec3{0, 0, 1}, image);
    TASK_CHECK(record->pixels.size() > 3);

    // Independently re-derive clamped bilinear sampling in the test and compare
    // the renderer's linear RGB against it (catches channel-order mistakes).
    for (const auto &p : record->pixels) {
        const double uc = std::min(1.0, std::max(0.0, p.u));
        const double vc = std::min(1.0, std::max(0.0, p.v));
        const double fx = uc, fy = vc; // a 2x2 texture maps [0,1] onto [0,1]
        const double wgt[4] = {(1-fx)*(1-fy), fx*(1-fy), (1-fx)*fy, fx*fy};
        const int cell[4][2] = {{0,0},{1,0},{0,1},{1,1}};
        const int pi = p.x + p.y * IMG_W;
        TASK_CHECK(image.face[pi] == 0);
        for (int c = 0; c < 3; ++c) {
            double expected_tex = 0;
            for (int t = 0; t < 4; ++t)
                expected_tex += wgt[t] * table[cell[t][1]][cell[t][0]][c] / 255.0;
            TASK_CHECK(close_enough(image.rgb[pi*3+c], expected_tex * p.shade));
        }
    }

    // Perspective correction: differing w, and beta must equal (lam/w)/sum.
    bool found_varied_w = false;
    for (const auto &p : record->pixels) {
        if (p.w[0] == p.w[1] && p.w[1] == p.w[2]) continue;
        found_varied_w = true;
        const double h[3] = {p.lam[0]/p.w[0], p.lam[1]/p.w[1], p.lam[2]/p.w[2]};
        const double s = h[0]+h[1]+h[2];
        for (int k = 0; k < 3; ++k)
            TASK_CHECK(std::fabs(p.beta[k] - h[k]/s) < 1e-12);
        // beta and screen barycentrics genuinely differ under perspective.
        TASK_CHECK(std::fabs(p.beta[0]-p.lam[0]) + std::fabs(p.beta[1]-p.lam[1]) > 1e-9);
    }
    TASK_CHECK(found_varied_w);
    return 0;
}

static int forward_occlusion_background_and_ties() {
    const fs::path stem = work_dir() / "occ";
    // Far quad (two triangles), a nearer small triangle in front of its centre,
    // and a fully hidden triangle behind it with exclusive vertices.
    write_obj(obj_path(stem),
              {{-0.7,-0.7,0,1}, {0.7,-0.7,0,1}, {0.7,0.7,0,1}, {-0.7,0.7,0,1},
               {-0.3,-0.3,1.5,1}, {0.3,-0.3,1.5,1}, {0,0.3,1.5,1},
               {-0.25,-0.25,-1.5,1}, {0.25,-0.25,-1.5,1}, {0,0.25,-1.5,1}},
              {{0,0},{1,0},{1,1},{0,1}, {0.25,0.25},{0.75,0.25},{0.5,0.75},
               {0.2,0.2},{0.8,0.2},{0.5,0.8}},
              {{1,1,2,2,3,3}, {1,1,3,3,4,4},
               {5,5,6,6,7,7}, {8,8,9,9,10,10}});
    WRITE_TEX(tex_path(stem), 4, 3,
                  [](int x, int y, std::uint8_t rgb[3]) {
                      rgb[0] = static_cast<std::uint8_t>(40 + x*30);
                      rgb[1] = static_cast<std::uint8_t>(80 + y*40);
                      rgb[2] = static_cast<std::uint8_t>(160 - x*10 - y*10);
                  });
    setup_camera();
    Model model(obj_path(stem).string());
    auto scene = DiffScene::from_model(model, IMG_W, IMG_H);
    DiffImage image;
    auto record = diff_render(*scene, vec3{0, 0, 1}, image);

    int near_pixels = 0, far_pixels = 0, background = 0, near_sample = -1;
    for (int y = 0; y < IMG_H; ++y)
        for (int x = 0; x < IMG_W; ++x) {
            const int pi = x + y*IMG_W;
            if (image.face[pi] < 0) {
                ++background;
                TASK_CHECK(image.rgb[pi*3+0] == 0.0 && image.rgb[pi*3+1] == 0.0 &&
                           image.rgb[pi*3+2] == 0.0 && image.depth[pi] == 0.0);
            } else if (image.face[pi] == 2) {
                ++near_pixels;
                near_sample = pi;
            } else if (image.face[pi] < 2) {
                ++far_pixels;
            }
            TASK_CHECK(image.face[pi] != 3); // the back triangle never wins
        }
    TASK_CHECK(near_pixels > 0 && far_pixels > 0 && background > 0);

    // A winning near pixel must carry the near face's shaded texture.
    const DiffPixelRecord *near_pr = find_pixel_record(*record, near_sample);
    TASK_CHECK(near_pr != nullptr && near_pr->face == 2);
    for (int c = 0; c < 3; ++c) {
        double tex = 0;
        const double wb[4] = {(1-near_pr->fx)*(1-near_pr->fy), near_pr->fx*(1-near_pr->fy),
                              (1-near_pr->fx)*near_pr->fy,     near_pr->fx*near_pr->fy};
        for (int t = 0; t < 4; ++t) tex += wb[t] * near_pr->tex[t][c];
        TASK_CHECK(close_enough(image.rgb[near_sample*3+c], tex * near_pr->shade));
    }

    // Equal depth keeps the face drawn first: a second, smaller coplanar
    // triangle fully inside the first never overwrites any of its pixels.
    const fs::path tie_stem = work_dir() / "tie";
    write_obj(obj_path(tie_stem),
              {{-1.2,-1.2,0,1}, {1.2,-1.2,0,1}, {0,1.2,0,1},
               {-1.1,-1.1,0,1}, {1.1,-1.1,0,1}, {0,1.1,0,1}},
              {{0,0},{1,0},{0.5,1}, {0,0},{1,0},{0.5,1}},
              {{1,1,2,2,3,3}, {4,4,5,5,6,6}});
    WRITE_TEX(tex_path(tie_stem), 2, 2,
                  [](int, int, std::uint8_t rgb[3]) { rgb[0]=255; rgb[1]=255; rgb[2]=255; });
    Model tie_model(obj_path(tie_stem).string());
    auto tie_scene = DiffScene::from_model(tie_model, IMG_W, IMG_H);
    DiffImage tie_image;
    auto tie_record = diff_render(*tie_scene, vec3{0,0,1}, tie_image);
    TASK_CHECK(tie_record->pixels.size() > 0);
    for (const auto &p : tie_record->pixels) TASK_CHECK(p.face == 0);
    for (int i = 0; i < IMG_W*IMG_H; ++i) TASK_CHECK(tie_image.face[i] != 1);
    return 0;
}

static int forward_corner_uvs_are_not_merged() {
    const fs::path stem = work_dir() / "seam";
    // Two corners share one position but point at different vt entries.
    write_obj(obj_path(stem),
              {{-1,-1,0,1}, {1,-1,0,1}, {0,1,0,1}},
              {{0.1,0.1}, {0.9,0.2}, {0.5,0.8}, {0.2,0.9}},
              {{1,1,2,2,3,3}, {1,4,2,2,3,3}});
    WRITE_TEX(tex_path(stem), 4, 3,
                  [](int, int, std::uint8_t rgb[3]) { rgb[0]=128; rgb[1]=128; rgb[2]=128; });
    setup_camera();
    Model model(obj_path(stem).string());
    auto scene = DiffScene::from_model(model, IMG_W, IMG_H);
    TASK_CHECK(scene->num_corner_uvs() == 6);
    TASK_CHECK(scene->face_position_index(0, 0) == scene->face_position_index(1, 0));
    TASK_CHECK(std::fabs(scene->uv(0, 0).x - scene->uv(1, 0).x) > 1e-9);
    TASK_CHECK(scene->num_positions() == 3); // positions stay shared
    return 0;
}

// ---------------------------------------------------------------------------
// adjoint group
// ---------------------------------------------------------------------------

namespace {

struct AdjointFixture {
    fs::path stem;
    std::unique_ptr<DiffScene> scene;
    vec3 light{0, 0, 1};
    int pixel = 0;
};

bool make_single_triangle_fixture(const vec3 &light, AdjointFixture &fx) {
    fx = AdjointFixture{};
    fx.stem = work_dir() / "adj";
    write_obj(obj_path(fx.stem),
              {{-1.2,-1.2,-0.4,1}, {1.2,-1.2,0.3,1}, {0,1.2,0.1,1}},
              {{0.12,0.18}, {0.82,0.24}, {0.46,0.86}},
              {{1,1,2,2,3,3}});
    if (!write_texture(tex_path(fx.stem), 4, 3,
                  [](int x, int y, std::uint8_t rgb[3]) {
                      rgb[0] = static_cast<std::uint8_t>(30 + x*50 + y*7);
                      rgb[1] = static_cast<std::uint8_t>(90 + y*40 - x*6);
                      rgb[2] = static_cast<std::uint8_t>(200 - x*20 - y*15);
                  })) return false;
    setup_camera();
    Model model(obj_path(fx.stem).string());
    fx.scene = DiffScene::from_model(model, IMG_W, IMG_H);
    fx.light = light;

    DiffImage image;
    auto record = diff_render(*fx.scene, light, image);
    // Pick a pixel well away from coverage, face-selection and sampling kinks.
    const DiffPixelRecord *pick = nullptr;
    for (const auto &p : record->pixels) {
        const bool interior = p.lam[0] > 0.2 && p.lam[1] > 0.2 && p.lam[2] > 0.2;
        const bool sample_safe = p.fx > 0.15 && p.fx < 0.85 && p.fy > 0.15 && p.fy < 0.85 &&
                                 p.u > 0.05 && p.u < 0.95 && p.v > 0.05 && p.v < 0.95;
        if (interior && sample_safe) { pick = &p; break; }
    }
    if (pick == nullptr) return false;
    fx.pixel = pick->x + pick->y * IMG_W;
    return true;
}

} // namespace

static int adjoint_matches_finite_differences() {
    AdjointFixture fx;
    TASK_CHECK(make_single_triangle_fixture(vec3{0, 0, 1}, fx));
    DiffScene &scene = *fx.scene;
    DiffImage image;
    auto record = diff_render(scene, fx.light, image);
    const DiffPixelRecord *pr = find_pixel_record(*record, fx.pixel);
    TASK_CHECK(pr != nullptr);
    TASK_CHECK(pr->lit); // exercise the unit-normal -> shade -> color chain, not just ambient
    const auto g = loss_adjoint(*record, fx.pixel);
    const DiffGradients analytic = diff_backward(*record, g);

    // A texel guaranteed to participate in this pixel's bilinear blend.
    const int flat_cell = pr->tex_cell[0];
    const int tx = flat_cell % scene.texture_width();
    const int ty = flat_cell / scene.texture_width();

    const double epsilons[2] = {1e-3, 1e-4};
    for (const double eps : epsilons) {
        // Position components (exclusive vertices here): projection + normal chain.
        for (int vi = 0; vi < 3; ++vi)
            for (int axis = 0; axis < 3; ++axis) {
                const double saved = scene.position(vi)[axis];
                scene.position(vi)[axis] = saved + eps;
                const double lp = pixel_loss(scene, fx.light, fx.pixel);
                scene.position(vi)[axis] = saved - eps;
                const double lm = pixel_loss(scene, fx.light, fx.pixel);
                scene.position(vi)[axis] = saved;
                TASK_CHECK(close_enough(analytic.positions[vi][axis], (lp-lm)/(2*eps)));
            }
        // Seam corner UVs: each face corner is its own parameter.
        for (int corner = 0; corner < 3; ++corner)
            for (int axis = 0; axis < 2; ++axis) {
                const double saved = scene.uv(0, corner)[axis];
                scene.uv(0, corner)[axis] = saved + eps;
                const double lp = pixel_loss(scene, fx.light, fx.pixel);
                scene.uv(0, corner)[axis] = saved - eps;
                const double lm = pixel_loss(scene, fx.light, fx.pixel);
                scene.uv(0, corner)[axis] = saved;
                TASK_CHECK(close_enough(analytic.uvs[corner][axis], (lp-lm)/(2*eps)));
            }
        // One participating RGB texel.
        for (int c = 0; c < 3; ++c) {
            const double saved = scene.texel(tx, ty, c);
            scene.texel(tx, ty, c) = saved + eps;
            const double lp = pixel_loss(scene, fx.light, fx.pixel);
            scene.texel(tx, ty, c) = saved - eps;
            const double lm = pixel_loss(scene, fx.light, fx.pixel);
            scene.texel(tx, ty, c) = saved;
            TASK_CHECK(close_enough(analytic.texture[(ty*scene.texture_width()+tx)*3+c],
                                    (lp-lm)/(2*eps)));
        }
    }
    return 0;
}

static int adjoint_shared_positions_add() {
    // Shared-vertex quad versus the same geometry with every corner duplicated.
    const auto tex_color = [](int x, int y, std::uint8_t rgb[3]) {
        rgb[0]=static_cast<std::uint8_t>(50+x*40);
        rgb[1]=static_cast<std::uint8_t>(70+y*50);
        rgb[2]=static_cast<std::uint8_t>(180-x*15);
    };
    const fs::path shared_stem = work_dir() / "shared";
    write_obj(obj_path(shared_stem),
              {{-1,-1,0,1}, {1,-1,0.2,1}, {1,1,0,1}, {-1,1,-0.2,1}},
              {{0,0},{1,0},{1,1},{0,1}},
              {{1,1,2,2,3,3}, {1,1,3,3,4,4}});
    WRITE_TEX(tex_path(shared_stem), 4, 3, tex_color);
    const fs::path dup_stem = work_dir() / "dup";
    write_obj(obj_path(dup_stem),
              {{-1,-1,0,1}, {1,-1,0.2,1}, {1,1,0,1},
               {-1,-1,0,1}, {1,1,0,1}, {-1,1,-0.2,1}},
              {{0,0},{1,0},{1,1}, {0,0},{1,1},{0,1}},
              {{1,1,2,2,3,3}, {4,4,5,5,6,6}});
    WRITE_TEX(tex_path(dup_stem), 4, 3, tex_color);
    setup_camera();
    Model shared_model(obj_path(shared_stem).string());
    Model dup_model(obj_path(dup_stem).string());
    auto shared = DiffScene::from_model(shared_model, IMG_W, IMG_H);
    auto dup    = DiffScene::from_model(dup_model, IMG_W, IMG_H);

    DiffImage img_s, img_d;
    auto rec_s = diff_render(*shared, vec3{0,0,1}, img_s);
    auto rec_d = diff_render(*dup, vec3{0,0,1}, img_d);
    TASK_CHECK(rec_s->pixels.size() == rec_d->pixels.size());

    std::vector<double> g(IMG_W*IMG_H*3, 0.0);
    for (const auto &p : rec_s->pixels) {
        const int pi = p.x + p.y*IMG_W;
        g[pi*3] = g[pi*3+1] = g[pi*3+2] = 0.7;
    }
    const DiffGradients gs = diff_backward(*rec_s, g);
    const DiffGradients gd = diff_backward(*rec_d, g);

    // Shared vertex 0 (both faces) equals duplicated vertices 0+3; shared
    // vertex 2 equals duplicated 2+4.
    for (int axis = 0; axis < 3; ++axis) {
        TASK_CHECK(std::fabs(gs.positions[0][axis] -
                             (gd.positions[0][axis] + gd.positions[3][axis])) < 1e-10);
        TASK_CHECK(std::fabs(gs.positions[2][axis] -
                             (gd.positions[2][axis] + gd.positions[4][axis])) < 1e-10);
    }
    return 0;
}

static int adjoint_invisible_and_clamp_are_zero() {
    const fs::path stem = work_dir() / "invis";
    write_obj(obj_path(stem),
              {{-1.2,-1.2,0,1}, {1.2,-1.2,0,1}, {1.2,1.2,0,1}, {-1.2,1.2,0,1},
               {-0.3,-0.3,1.5,1}, {0.3,-0.3,1.5,1}, {0,0.3,1.5,1},
               {-0.25,-0.25,-1.5,1}, {0.25,-0.25,-1.5,1}, {0,0.25,-1.5,1}},
              {{-0.2,-0.2},{1.2,-0.2},{1.2,1.2},{-0.2,1.2}, {0.3,0.3},{0.7,0.3},{0.5,0.7},
               {0.3,0.3},{0.7,0.3},{0.5,0.7}},
              {{1,1,2,2,3,3}, {1,1,3,3,4,4}, {5,5,6,6,7,7}, {8,8,9,9,10,10}});
    WRITE_TEX(tex_path(stem), 4, 3,
                  [](int x, int y, std::uint8_t rgb[3]) {
                      rgb[0]=static_cast<std::uint8_t>(60+x*30);
                      rgb[1]=static_cast<std::uint8_t>(110+y*30);
                      rgb[2]=static_cast<std::uint8_t>(210-x*20);
                  });
    setup_camera();
    Model model(obj_path(stem).string());
    auto scene = DiffScene::from_model(model, IMG_W, IMG_H);
    DiffImage image;
    auto record = diff_render(*scene, vec3{0,0,1}, image);

    std::vector<double> g(IMG_W*IMG_H*3, 1.0); // nonzero even on background pixels
    const DiffGradients grad = diff_backward(*record, g);

    // Face 3 is fully occluded: its exclusive positions and corner UVs are zero.
    for (int vi = 7; vi < 10; ++vi)
        for (int axis = 0; axis < 4; ++axis)
            TASK_CHECK(grad.positions[vi][axis] == 0.0);
    for (int corner = 9; corner < 12; ++corner)
        TASK_CHECK(grad.uvs[corner].x == 0.0 && grad.uvs[corner].y == 0.0);

    // A pixel clamped at a UV endpoint gets a zero UV derivative, including at
    // the endpoint itself. Back-propagate through that pixel alone so other
    // pixels of the same face cannot mask the zero contribution.
    const DiffPixelRecord *endp = nullptr;
    for (const auto &p : record->pixels)
        if (p.u <= 0.0 || p.v <= 0.0 || p.u >= 1.0 || p.v >= 1.0) { endp = &p; break; }
    TASK_CHECK(endp != nullptr);
    const bool u_clamped = (endp->u <= 0.0 || endp->u >= 1.0);
    const bool v_clamped = (endp->v <= 0.0 || endp->v >= 1.0);
    std::vector<double> g_one(IMG_W*IMG_H*3, 0.0);
    const int endp_index = endp->x + endp->y * IMG_W;
    g_one[endp_index*3] = g_one[endp_index*3+1] = g_one[endp_index*3+2] = 1.0;
    const DiffGradients grad_endp = diff_backward(*record, g_one);
    for (int k = 0; k < 3; ++k) {
        if (u_clamped) TASK_CHECK(grad_endp.uvs[endp->face*3+k].x == 0.0);
        if (v_clamped) TASK_CHECK(grad_endp.uvs[endp->face*3+k].y == 0.0);
    }
    return 0;
}

static int adjoint_max_kink_and_lighting_chain() {
    AdjointFixture fx;
    TASK_CHECK(make_single_triangle_fixture(vec3{0, 0, 1}, fx));
    DiffImage probe;
    auto probe_rec = diff_render(*fx.scene, vec3{0,0,1}, probe);
    vec3 N{};
    for (const auto &p : probe_rec->pixels) {
        N = {probe_rec->faces[p.face].N[0],
             probe_rec->faces[p.face].N[1],
             probe_rec->faces[p.face].N[2]};
        break;
    }
    const vec3 away = normalized(vec3{-N.x, -N.y, -N.z});

    // A unit light exactly perpendicular to the face normal: N.L == 0.
    const vec3 helper = std::fabs(N.x) < 0.9 ? vec3{1,0,0} : vec3{0,1,0};
    const vec3 perp = normalized(cross(N, helper));
    TASK_CHECK(std::fabs(N.x*perp.x + N.y*perp.y + N.z*perp.z) < 1e-12);

    const auto g = [&](const DiffRecord &r) { return loss_adjoint(r, fx.pixel); };

    // --- Deep back light: N.L = -1, a smoothly differentiable ambient-only region.
    fx.light = away;
    DiffImage img_away;
    auto rec_away = diff_render(*fx.scene, away, img_away);
    const DiffPixelRecord *pr_away = find_pixel_record(*rec_away, fx.pixel);
    TASK_CHECK(pr_away != nullptr && !pr_away->lit);
    TASK_CHECK(std::fabs(pr_away->shade - 0.2) < 1e-12);
    const DiffGradients grad_away = diff_backward(*rec_away, g(*rec_away));
    // Two-sided FD stays on the negative side, so it measures the ambient-only
    // slope and must match the analytic (lighting-independent) gradient.
    const double eps = 1e-4;
    for (int vi = 0; vi < 3; ++vi)
        for (int axis = 0; axis < 3; ++axis) {
            const double saved = fx.scene->position(vi)[axis];
            fx.scene->position(vi)[axis] = saved + eps;
            const double lp = pixel_loss(*fx.scene, away, fx.pixel);
            fx.scene->position(vi)[axis] = saved - eps;
            const double lm = pixel_loss(*fx.scene, away, fx.pixel);
            fx.scene->position(vi)[axis] = saved;
            TASK_CHECK(close_enough(grad_away.positions[vi][axis], (lp-lm)/(2*eps)));
        }

    // --- Exact kink N.L == 0: lit must be false and shade stays at ambient.
    DiffImage img_perp;
    auto rec_perp = diff_render(*fx.scene, perp, img_perp);
    const DiffPixelRecord *pr_perp = find_pixel_record(*rec_perp, fx.pixel);
    TASK_CHECK(pr_perp != nullptr && !pr_perp->lit);
    TASK_CHECK(std::fabs(pr_perp->shade - 0.2) < 1e-12);
    const DiffGradients grad_perp = diff_backward(*rec_perp, g(*rec_perp));

    // At the kink the max derivative is zero, so the full adjoint must be
    // element-for-element identical to the deep-back-light (ambient-only) one:
    // positions, per-corner UVs and every texel.
    TASK_CHECK(grad_perp.positions.size() == grad_away.positions.size());
    TASK_CHECK(grad_perp.uvs.size() == grad_away.uvs.size());
    TASK_CHECK(grad_perp.texture.size() == grad_away.texture.size());
    for (size_t i = 0; i < grad_perp.positions.size(); ++i)
        for (int a = 0; a < 4; ++a)
            TASK_CHECK(grad_perp.positions[i][a] == grad_away.positions[i][a]);
    for (size_t i = 0; i < grad_perp.uvs.size(); ++i)
        TASK_CHECK(grad_perp.uvs[i].x == grad_away.uvs[i].x &&
                   grad_perp.uvs[i].y == grad_away.uvs[i].y);
    for (size_t i = 0; i < grad_perp.texture.size(); ++i)
        TASK_CHECK(grad_perp.texture[i] == grad_away.texture[i]);
    return 0;
}

// ---------------------------------------------------------------------------
// lifetime group
// ---------------------------------------------------------------------------

static int lifetime_records_are_independent() {
    const auto tex_a = [](int x, int y, std::uint8_t rgb[3]) {
        rgb[0]=static_cast<std::uint8_t>(40+x*40);
        rgb[1]=static_cast<std::uint8_t>(90+y*30);
        rgb[2]=static_cast<std::uint8_t>(200-x*25);
    };
    const auto tex_b = [](int x, int y, std::uint8_t rgb[3]) {
        rgb[0]=static_cast<std::uint8_t>(10+x*60);
        rgb[1]=static_cast<std::uint8_t>(120+y*40);
        rgb[2]=static_cast<std::uint8_t>(170-x*30);
    };
    const fs::path stem_a = work_dir() / "life_a";
    write_obj(obj_path(stem_a),
              {{-1,-1,0,1}, {1,-1,0.2,1}, {0,1,0,1}},
              {{0.1,0.1},{0.9,0.2},{0.5,0.8}},
              {{1,1,2,2,3,3}});
    WRITE_TEX(tex_path(stem_a), 4, 3, tex_a);
    const fs::path stem_b = work_dir() / "life_b";
    write_obj(obj_path(stem_b),
              {{-0.8,-0.8,-0.3,1}, {0.9,-0.8,0.4,1}, {0.1,0.9,0.2,1}},
              {{0.2,0.15},{0.75,0.3},{0.4,0.75}},
              {{1,1,2,2,3,3}});
    WRITE_TEX(tex_path(stem_b), 3, 2, tex_b);

    setup_camera();
    std::unique_ptr<DiffRecord> rec_a, rec_b;
    std::unique_ptr<DiffScene> scene_a, scene_b;
    {
        Model model_a(obj_path(stem_a).string());
        Model model_b(obj_path(stem_b).string());
        scene_a = DiffScene::from_model(model_a, IMG_W, IMG_H);
        scene_b = DiffScene::from_model(model_b, IMG_W, IMG_H);
        DiffImage img_a, img_b;
        rec_a = diff_render(*scene_a, vec3{0,0,1}, img_a);
        rec_b = diff_render(*scene_b, vec3{0.2,0.3,0.9}, img_b);
    } // both Models die here; the records must keep everything they need

    std::vector<double> g_a(IMG_W*IMG_H*3, 0.0), g_b(IMG_W*IMG_H*3, 0.0);
    for (const auto &p : rec_a->pixels) {
        const int pi = p.x + p.y*IMG_W;
        g_a[pi*3] = 0.5; g_a[pi*3+1] = 0.6; g_a[pi*3+2] = 0.7;
    }
    for (const auto &p : rec_b->pixels) {
        const int pi = p.x + p.y*IMG_W;
        g_b[pi*3] = 0.9; g_b[pi*3+1] = 0.4; g_b[pi*3+2] = 0.2;
    }

    const DiffGradients first_a = diff_backward(*rec_a, g_a);
    const DiffGradients first_b = diff_backward(*rec_b, g_b);

    // Mutate the scenes, move the global camera, and render another frame.
    for (int i = 0; i < scene_a->num_positions(); ++i) scene_a->position(i).x += 0.37;
    scene_a->uv(0, 0).x = 0.77;
    scene_a->texel(0, 0, 0) = 0.123;
    lookat({2, 1, 5}, {0.1, 0.2, 0.3}, {0, 1, 0});
    init_perspective(1.7);
    init_viewport(2, 2, IMG_W-4, IMG_H-4);
    DiffImage other;
    auto extra = diff_render(*scene_b, vec3{0,0,1}, other);
    (void)extra;

    const DiffGradients second_a = diff_backward(*rec_a, g_a);
    const DiffGradients second_b = diff_backward(*rec_b, g_b);
    const DiffGradients third_a  = diff_backward(*rec_a, g_a);

    auto same_grad = [](const DiffGradients &x, const DiffGradients &y) {
        if (x.positions.size() != y.positions.size() ||
            x.uvs.size() != y.uvs.size() || x.texture.size() != y.texture.size()) return false;
        for (size_t i = 0; i < x.positions.size(); ++i)
            for (int a = 0; a < 4; ++a)
                if (x.positions[i][a] != y.positions[i][a]) return false;
        for (size_t i = 0; i < x.uvs.size(); ++i)
            if (x.uvs[i].x != y.uvs[i].x || x.uvs[i].y != y.uvs[i].y) return false;
        for (size_t i = 0; i < x.texture.size(); ++i)
            if (x.texture[i] != y.texture[i]) return false;
        return true;
    };
    TASK_CHECK(same_grad(first_a, second_a));
    TASK_CHECK(same_grad(first_a, third_a));
    TASK_CHECK(same_grad(first_b, second_b));

    // Every backward call starts from zero: corrupt one result and re-run.
    DiffGradients corrupted = second_a;
    corrupted.texture[0] += 5.0;
    corrupted.positions[0].x += 5.0;
    const DiffGradients clean = diff_backward(*rec_a, g_a);
    TASK_CHECK(same_grad(clean, first_a));

    // Different g gives a different, independent result.
    std::vector<double> g_other = g_a;
    g_other[rec_a->pixels.front().x*3] = 2.5;
    const DiffGradients other_grad = diff_backward(*rec_a, g_other);
    bool differs = false;
    for (size_t i = 0; i < other_grad.texture.size(); ++i)
        if (other_grad.texture[i] != first_a.texture[i]) { differs = true; break; }
    TASK_CHECK(differs);
    return 0;
}

extern const struct task_case task_cases[] = {
    {"forward",   "perspective_and_rgb_order",        &forward_perspective_and_rgb_order},
    {"forward",   "occlusion_background_and_ties",    &forward_occlusion_background_and_ties},
    {"forward",   "corner_uvs_are_not_merged",        &forward_corner_uvs_are_not_merged},
    {"adjoint",   "matches_finite_differences",       &adjoint_matches_finite_differences},
    {"adjoint",   "shared_positions_add",             &adjoint_shared_positions_add},
    {"adjoint",   "invisible_and_clamp_are_zero",     &adjoint_invisible_and_clamp_are_zero},
    {"adjoint",   "max_kink_and_lighting_chain",      &adjoint_max_kink_and_lighting_chain},
    {"lifetime",  "records_are_independent",          &lifetime_records_are_independent},
};
extern const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
