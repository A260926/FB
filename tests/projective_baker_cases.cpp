#include "task_support/check.h"
#include "projective_baker.h"
#include "our_gl.h"
#include "tgaimage.h"
#include <cmath>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

using projective_baker::BakeResult;
using projective_baker::OverlapConflict;
using projective_baker::ProjectiveGeometry;
using projective_baker::SourceCapture;
using projective_baker::bake_atlas;
using projective_baker::bake_error;

namespace {

bool near(const double a, const double b, const double eps = 1e-10) {
    return std::abs(a - b) <= eps;
}

double cross2(const vec2 &a, const vec2 &b) {
    return a.x * b.y - a.y * b.x;
}

mat<4, 4> identity4() {
    mat<4, 4> m{};
    for (int i = 0; i < 4; ++i) m[i][i] = 1.;
    return m;
}

// Perspective matrix of our_gl with f=1 acting on raw world points:
// clip = (x, y, z, 1-z), so vertices at different z get different w.
mat<4, 4> perspective_f1() {
    mat<4, 4> m = identity4();
    m[3][2] = -1.;
    return m;
}

// Writes minimal triangulated OBJ files (plus dummy textures so Model does
// not report missing files). OBJ vt y is flipped by Model, so the helper
// takes already-flipped Model-space UVs directly.
struct ObjScene {
    std::filesystem::path dir;
    std::vector<std::string> model_paths;

    struct V { double x = 0., y = 0., z = 0.; };
    struct F { int v[3] = {}, t[3] = {}; };

    static F face(int a, int b, int c) {
        F f{};
        f.v[0] = a; f.v[1] = b; f.v[2] = c;
        f.t[0] = a; f.t[1] = b; f.t[2] = c;
        return f;
    }
    static F face_uv(const int v[3], const int t[3]) {
        F f{};
        for (int i = 0; i < 3; ++i) { f.v[i] = v[i]; f.t[i] = t[i]; }
        return f;
    }

    static ObjScene create(const std::string &name,
                           const std::vector<std::vector<V>> &all_verts,
                           const std::vector<std::vector<V>> &all_uvs,
                           const std::vector<std::vector<F>> &all_faces) {
        ObjScene scene;
        scene.dir = std::filesystem::temp_directory_path() / ("proj_baker_" + name);
        std::filesystem::remove_all(scene.dir);
        std::filesystem::create_directories(scene.dir);
        for (size_t m = 0; m < all_verts.size(); ++m) {
            std::string base = "mesh" + std::to_string(m);
            std::filesystem::path obj = scene.dir / (base + ".obj");
            std::ofstream out(obj);
            for (const V &p : all_verts[m]) out << "v " << p.x << ' ' << p.y << ' ' << p.z << '\n';
            for (const V &t : all_uvs[m]) out << "vt " << t.x << ' ' << (1. - t.y) << '\n';
            out << "vn 0 0 1\n";
            for (const F &f : all_faces[m])
                out << "f " << f.v[0] + 1 << '/' << f.t[0] + 1 << "/1 "
                    << f.v[1] + 1 << '/' << f.t[1] + 1 << "/1 "
                    << f.v[2] + 1 << '/' << f.t[2] + 1 << "/1\n";
            out.close();
            TGAImage dif(2, 2, TGAImage::RGB, {{10, 20, 30, 255}, 3});
            TGAImage nm(2, 2, TGAImage::RGB, {{255, 128, 128, 255}, 3});
            TGAImage sp(2, 2, TGAImage::GRAYSCALE, {{0, 0, 0, 0}, 1});
            dif.write_tga_file((scene.dir / (base + "_diffuse.tga")).string());
            nm.write_tga_file((scene.dir / (base + "_nm_tangent.tga")).string());
            sp.write_tga_file((scene.dir / (base + "_spec.tga")).string());
            scene.model_paths.push_back(obj.string());
        }
        return scene;
    }
};

std::vector<double> uniform_image(const int w, const int h,
                                  const double r, const double g, const double b) {
    std::vector<double> img(static_cast<size_t>(w) * h * 3u);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            size_t p = (static_cast<size_t>(y) * w + x) * 3u;
            img[p] = r; img[p + 1] = g; img[p + 2] = b;
        }
    return img;
}

// Linear RGB image where every pixel stores its own center coordinates.
std::vector<double> coord_image(const int w, const int h) {
    std::vector<double> img(static_cast<size_t>(w) * h * 3u);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            size_t p = (static_cast<size_t>(y) * w + x) * 3u;
            img[p]     = (x + 0.5) / w;
            img[p + 1] = (y + 0.5) / h;
            img[p + 2] = 0.25;
        }
    return img;
}

// Independent test-side bilinear reference on row-major linear RGB images.
void bilinear_ref(const std::vector<double> &img, const int w, const int h,
                  const double s, const double t, double out[3], double &w_sum) {
    int ix = static_cast<int>(std::floor(s));
    int iy = static_cast<int>(std::floor(t));
    double fx = s - ix, fy = t - iy;
    double coeff[4] = {(1 - fx) * (1 - fy), fx * (1 - fy), (1 - fx) * fy, fx * fy};
    int xs[4] = {ix, ix + 1, ix, ix + 1};
    int ys[4] = {iy, iy, iy + 1, iy + 1};
    out[0] = out[1] = out[2] = 0.;
    w_sum = 0.;
    for (int k = 0; k < 4; ++k) {
        if (xs[k] < 0 || ys[k] < 0 || xs[k] >= w || ys[k] >= h) continue;
        size_t p = (static_cast<size_t>(ys[k]) * w + xs[k]) * 3u;
        for (int c = 0; c < 3; ++c) out[c] += coeff[k] * img[p + c];
        w_sum += coeff[k];
    }
}

// UV-space barycentrics of a texel center for one model face (test-side reference).
bool uv_barycentric(const Model &model, const int face, const vec2 &center, double lambda[3]) {
    vec2 uv[3] = {model.uv(face, 0), model.uv(face, 1), model.uv(face, 2)};
    double den = cross2(uv[1] - uv[0], uv[2] - uv[0]);
    if (den == 0.) return false;
    lambda[0] = cross2(uv[1] - center, uv[2] - center) / den;
    lambda[1] = cross2(uv[2] - center, uv[0] - center) / den;
    lambda[2] = 1. - lambda[0] - lambda[1];
    return lambda[0] >= 0. && lambda[1] >= 0. && lambda[2] >= 0.;
}

vec3 reconstruct_point(const Model &model, const int face, const double lambda[3]) {
    vec3 p{};
    for (int k = 0; k < 3; ++k) {
        vec3 v = model.vert(face, k).xyz();
        p = p + v * lambda[k];
    }
    return p;
}

vec3 geometric_normal(const Model &model, const int face) {
    vec3 a = model.vert(face, 0).xyz();
    vec3 b = model.vert(face, 1).xyz();
    vec3 c = model.vert(face, 2).xyz();
    return normalized(cross(b - a, c - a));
}

// pow(max(0, N.normalize(eye-P)), 4), exactly the baker's facing factor.
double facing_factor4(const vec3 &p, const vec3 &n, const vec3 &eye) {
    vec3 d = eye - p;
    double len = norm(d);
    if (!(len > 0.)) return 0.;
    return std::pow(std::max(0., n * (d / len)), 4.);
}

// Analytic projection of a world point through a world-to-clip matrix.
void project_point(const mat<4, 4> &m, const vec3 &p, double &s, double &t,
                   const int width, const int height) {
    vec4 clip = m * vec4{p.x, p.y, p.z, 1.};
    vec4 ndc = clip / clip.w;
    s = (ndc.x + 1.) * width / 2.;
    t = (ndc.y + 1.) * height / 2.;
}

// Rectangle at z=0 spanning world [-0.5,0.5]^2, UVs covering the whole atlas.
ObjScene make_rect_scene(const std::string &name) {
    using V = ObjScene::V;
    using F = ObjScene::F;
    std::vector<std::vector<V>> verts = {{ {-.5, -.5, 0.}, {.5, -.5, 0.},
                                           {.5, .5, 0.}, {-.5, .5, 0.} }};
    std::vector<std::vector<V>> uvs = {{ {0., 0.}, {1., 0.}, {1., 1.}, {0., 1.} }};
    std::vector<std::vector<F>> faces(1);
    faces[0].push_back(ObjScene::face(0, 1, 2));
    faces[0].push_back(ObjScene::face(0, 2, 3));
    return ObjScene::create(name, verts, uvs, faces);
}

// ---------------------------------------------------------------- projection

int case_ortho_color_and_weight() {
    // Inset UVs so the atlas also has genuinely uncovered texels.
    using V = ObjScene::V;
    using F = ObjScene::F;
    std::vector<std::vector<V>> verts = {{ {-.5, -.5, 0.}, {.5, -.5, 0.},
                                           {.5, .5, 0.}, {-.5, .5, 0.} }};
    std::vector<std::vector<V>> uvs = {{ {.25, .25}, {.75, .25}, {.75, .75}, {.25, .75} }};
    std::vector<std::vector<F>> faces(1);
    faces[0].push_back(ObjScene::face(0, 1, 2));
    faces[0].push_back(ObjScene::face(0, 2, 3));
    ObjScene scene = ObjScene::create("ortho", verts, uvs, faces);
    Model model(scene.model_paths[0]);
    ProjectiveGeometry geometry({&model});
    const int W = 16, H = 16;
    std::vector<double> img = uniform_image(W, H, 0.1, 0.2, 0.3);
    SourceCapture capture(geometry, identity4(), {0., 0., 2.}, img, W, H);
    std::vector<const SourceCapture *> sources = {&capture};
    std::vector<OverlapConflict> conflicts;
    BakeResult result = bake_atlas(geometry, sources, 8, 8, 0., &conflicts);

    TASK_CHECK(result.width == 8 && result.height == 8);
    TASK_CHECK(conflicts.empty());
    // Texel (4,4), center (.5625,.5625), lies inside the inset UV square and
    // projects to s=t=8.5 with all four taps valid.
    size_t texel = 4 + 4 * 8;
    TASK_CHECK(result.covered[texel] == 1);
    TASK_CHECK(result.owner_model[texel] == 0 && result.owner_face[texel] >= 0);
    double lambda[3];
    TASK_CHECK(uv_barycentric(model, result.owner_face[texel],
                              vec2{.5625, .5625}, lambda));
    vec3 point = reconstruct_point(model, result.owner_face[texel], lambda);
    vec3 normal = geometric_normal(model, result.owner_face[texel]);
    double expected_weight = facing_factor4(point, normal, vec3{0., 0., 2.});
    TASK_CHECK(near(result.weight[texel], expected_weight, 1e-12));
    TASK_CHECK(result.weight[texel] > 0.9 && result.weight[texel] < 1.);
    TASK_CHECK(near(result.rgb[texel * 3], 0.1) &&
               near(result.rgb[texel * 3 + 1], 0.2) &&
               near(result.rgb[texel * 3 + 2], 0.3));
    // The atlas corner stays completely empty: no coverage, no color, no weight.
    size_t corner = 7;
    TASK_CHECK(result.covered[corner] == 0 && result.owner_model[corner] == -1);
    TASK_CHECK(result.weight[corner] == 0. && result.rgb[corner * 3] == 0.);
    return 0;
}

int case_perspective_different_w() {
    using V = ObjScene::V;
    using F = ObjScene::F;
    // Three vertices at three different depths => three different clip w.
    std::vector<std::vector<V>> verts = {{ {-.5, -.5, .2}, {.5, -.5, -.2}, {0., .5, 0.} }};
    std::vector<std::vector<V>> uvs   = {{ {0., 0.}, {1., 0.}, {.5, 1.} }};
    std::vector<std::vector<F>> faces(1);
    faces[0].push_back(ObjScene::face(0, 1, 2));
    ObjScene scene = ObjScene::create("persp", verts, uvs, faces);
    Model model(scene.model_paths[0]);
    ProjectiveGeometry geometry({&model});

    mat<4, 4> clip_m = perspective_f1();
    // Sanity: the clip-space w values really differ.
    vec4 c0 = clip_m * vec4{-.5, -.5, .2, 1.};
    vec4 c1 = clip_m * vec4{.5, -.5, -.2, 1.};
    vec4 c2 = clip_m * vec4{0., .5, 0., 1.};
    TASK_CHECK(near(c0.w, .8) && near(c1.w, 1.2) && near(c2.w, 1.));

    const int W = 24, H = 24;
    std::vector<double> img = coord_image(W, H);
    SourceCapture capture(geometry, clip_m, {0., 0., 2.}, img, W, H);
    BakeResult result = bake_atlas(geometry, {&capture}, 8, 8, 1e-12, nullptr);

    // Independent reference: world P must be the plain UV-barycentric
    // interpolation, never the perspective-correct camera barycentrics.
    vec3 ref_normal = geometric_normal(model, 0);
    for (int ty = 3; ty <= 4; ++ty) {
        for (int tx = 3; tx <= 4; ++tx) {
            double u = (tx + .5) / 8., v = (ty + .5) / 8.;
            double l1 = u - .5 * v, l2 = v, l0 = 1. - l1 - l2;
            double px = l0 * -.5 + l1 * .5;
            double py = l0 * -.5 + l1 * -.5 + l2 * .5;
            double pz = l0 * .2 + l1 * -.2;
            vec4 clip = clip_m * vec4{px, py, pz, 1.};
            vec4 ndc = clip / clip.w;
            double s = (ndc.x + 1.) * W / 2., t = (ndc.y + 1.) * H / 2.;
            double expect[3], wsum_all;
            bilinear_ref(img, W, H, s, t, expect, wsum_all);
            // The reference must honor the same captured coverage mask: taps
            // with no committed depth do not participate.
            int ix = static_cast<int>(std::floor(s)), iy = static_cast<int>(std::floor(t));
            double fx = s - ix, fy = t - iy;
            double coeff[4] = {(1 - fx) * (1 - fy), fx * (1 - fy),
                               (1 - fx) * fy, fx * fy};
            int xs[4] = {ix, ix + 1, ix, ix + 1};
            int ys[4] = {iy, iy, iy + 1, iy + 1};
            double wsum = 0.;
            for (int k = 0; k < 4; ++k)
                if (capture.has_depth(xs[k], ys[k])) wsum += coeff[k];
            double facing = facing_factor4(vec3{px, py, pz}, ref_normal, vec3{0., 0., 2.});
            size_t idx = static_cast<size_t>(ty) * 8 + tx;
            TASK_CHECK(result.covered[idx] == 1);
            TASK_CHECK(near(result.weight[idx], wsum * facing, 1e-10));
            for (int c = 0; c < 3; ++c)
                TASK_CHECK(near(result.rgb[idx * 3 + c], expect[c], 1e-10));
        }
    }
    return 0;
}

int case_mirrored_uv_shared_edge() {
    using V = ObjScene::V;
    using F = ObjScene::F;
    // World rectangle split along the A-C diagonal. The second face mirrors
    // its UVs across that diagonal (clockwise UV winding, which the baker
    // normalizes to CCW instead of rejecting).
    std::vector<std::vector<V>> verts = {{ {-.5, -.5, 0.}, {.5, -.5, 0.},
                                           {.5, .5, 0.}, {-.5, .5, 0.} }};
    std::vector<std::vector<V>> uvs = {{ {0., 0.}, {1., 0.}, {1., 1.}, {0., 1.},
                                         {0., 1.} /*idx4: mirrored A, same position as D's UV*/ }};
    std::vector<std::vector<F>> faces(1);
    faces[0].push_back(ObjScene::face(0, 1, 2));          // A B C, normal UVs
    {
        int v[3] = {0, 2, 3}, t[3] = {4, 2, 0};
        faces[0].push_back(ObjScene::face_uv(v, t));      // A C D, clockwise (mirrored) UVs
    }
    ObjScene scene = ObjScene::create("mirror", verts, uvs, faces);
    Model model(scene.model_paths[0]);
    ProjectiveGeometry geometry({&model});
    const int W = 16, H = 16;
    std::vector<double> img = uniform_image(W, H, .7, .35, .05);
    SourceCapture capture(geometry, identity4(), {0., 0., 2.}, img, W, H);
    BakeResult result = bake_atlas(geometry, {&capture}, 8, 8, 0., nullptr);

    size_t side_one = 3 + 2 * 8;   // u>v: first face
    size_t side_two = 2 + 3 * 8;   // v>u: mirrored face
    TASK_CHECK(result.covered[side_one] && result.covered[side_two]);
    TASK_CHECK(result.owner_face[side_one] != result.owner_face[side_two]);
    for (size_t idx : {side_one, side_two}) {
        double lambda[3];
        vec2 center{((idx % 8) + .5) / 8., ((idx / 8) + .5) / 8.};
        TASK_CHECK(uv_barycentric(model, result.owner_face[idx], center, lambda));
        vec3 point = reconstruct_point(model, result.owner_face[idx], lambda);
        vec3 normal = geometric_normal(model, result.owner_face[idx]);
        double expected_weight = facing_factor4(point, normal, vec3{0., 0., 2.});
        TASK_CHECK(near(result.weight[idx], expected_weight, 1e-12));
        TASK_CHECK(near(result.rgb[idx * 3], .7) &&
                   near(result.rgb[idx * 3 + 1], .35) &&
                   near(result.rgb[idx * 3 + 2], .05));
    }
    // Texels on the shared diagonal belong to exactly one face (top-left rule),
    // but every diagonal texel is covered by one of the two faces.
    int covered_on_diagonal = 0;
    for (int ty = 0; ty < 8; ++ty) {
        for (int tx = 0; tx < 8; ++tx) {
            double u = (tx + .5) / 8., v = (ty + .5) / 8.;
            if (std::abs(u + v - 1.) < .09) {
                size_t idx = ty * 8 + tx;
                TASK_CHECK(result.covered[idx] == 1);
                ++covered_on_diagonal;
            }
        }
    }
    TASK_CHECK(covered_on_diagonal >= 4);
    return 0;
}

int case_overlap_rejected() {
    using V = ObjScene::V;
    using F = ObjScene::F;
    // Two separate models whose UV triangles genuinely overlap around (.5,.5).
    std::vector<std::vector<V>> verts = {
        { {0., 0., 0.}, {1., 0., 0.}, {0., 1., 0.} },
        { {0., 0., 5.}, {1., 0., 5.}, {0., 1., 5.} }};
    std::vector<std::vector<V>> uvs = {
        { {0., 0.}, {1., 0.}, {0., 1.} },
        { {.2, .2}, {1., .2}, {.2, 1.} }};
    std::vector<std::vector<F>> faces(2);
    faces[0].push_back(ObjScene::face(0, 1, 2));
    faces[1].push_back(ObjScene::face(0, 1, 2));
    ObjScene scene = ObjScene::create("overlap", verts, uvs, faces);
    Model m0(scene.model_paths[0]), m1(scene.model_paths[1]);
    ProjectiveGeometry geometry({&m0, &m1});
    const int W = 8, H = 8;
    std::vector<double> img = uniform_image(W, H, .9, .8, .7);
    SourceCapture capture(geometry, identity4(), {0., 0., 10.}, img, W, H);

    std::vector<OverlapConflict> conflicts;
    BakeResult result = bake_atlas(geometry, {&capture}, 8, 8, 0., &conflicts);
    TASK_CHECK(!conflicts.empty());
    bool found = false;
    for (const OverlapConflict &c : conflicts) {
        TASK_CHECK(c.model_a != c.model_b);
        if (c.x == 3 && c.y == 3) found = true; // texel center (.4375,.4375)
    }
    TASK_CHECK(found);
    size_t idx = 3 + 3 * 8;
    TASK_CHECK(result.covered[idx] == 1);          // coverage/ownership preserved
    TASK_CHECK(result.weight[idx] == 0.);          // submission order must not pick a color
    TASK_CHECK(result.rgb[idx * 3] == 0. &&
               result.rgb[idx * 3 + 1] == 0. &&
               result.rgb[idx * 3 + 2] == 0.);

    // Reversing submission order reports the same pair, still without coloring.
    ProjectiveGeometry reversed({&m1, &m0});
    std::vector<OverlapConflict> conflicts2;
    BakeResult result2 = bake_atlas(reversed, {&capture}, 8, 8, 0., &conflicts2);
    TASK_CHECK(!conflicts2.empty());
    TASK_CHECK(result2.weight[idx] == 0. && result2.rgb[idx * 3] == 0.);
    return 0;
}

int case_empty_sources_keep_coverage() {
    ObjScene scene = make_rect_scene("empty");
    Model model(scene.model_paths[0]);
    ProjectiveGeometry geometry({&model});
    BakeResult result = bake_atlas(geometry, {}, 8, 8, 0., nullptr);
    int covered = 0;
    for (size_t i = 0; i < static_cast<size_t>(8) * 8; ++i) {
        if (result.covered[i]) {
            ++covered;
            TASK_CHECK(result.owner_model[i] == 0 && result.owner_face[i] >= 0);
            TASK_CHECK(result.weight[i] == 0.);
            TASK_CHECK(result.rgb[i * 3] == 0. && result.rgb[i * 3 + 2] == 0.);
        }
    }
    TASK_CHECK(covered >= 40);
    return 0;
}

// --------------------------------------------------------------- visibility

struct OccluderScene {
    ObjScene scene;
    static constexpr int W = 16, H = 16;
};

// Receiver rectangle at z=0 spanning world [-0.5,0.5]^2 plus a foreground
// quad. In the 16x16 source image the quad covers integer pixels with x in
// {10}, y in {6,7}: its screen box is [10.25,11.75) x [5.75,7.75), so every
// covered pixel lies strictly inside both quads (no edge ambiguity).
ObjScene make_occluder_scene(const std::string &name, const double occluder_z = .5) {
    using V = ObjScene::V;
    using F = ObjScene::F;
    std::vector<std::vector<V>> verts = {
        { {-.5, -.5, 0.}, {.5, -.5, 0.}, {.5, .5, 0.}, {-.5, .5, 0.} },
        { {.28125, -.28125, occluder_z}, {.46875, -.28125, occluder_z},
          {.46875, -.03125, occluder_z}, {.28125, -.03125, occluder_z} }};
    std::vector<std::vector<V>> uvs = {
        { {0., 0.}, {1., 0.}, {1., 1.}, {0., 1.} },
        { {.9, .01}, {.99, .01}, {.99, .09}, {.9, .09} }};
    std::vector<std::vector<F>> faces(2);
    faces[0].push_back(ObjScene::face(0, 1, 2));
    faces[0].push_back(ObjScene::face(0, 2, 3));
    faces[1].push_back(ObjScene::face(0, 1, 2));
    faces[1].push_back(ObjScene::face(0, 2, 3));
    return ObjScene::create(name, verts, uvs, faces);
}

// Texel (6,2) of an 8x8 atlas (center .8125,.3125, strictly inside receiver
// face 0) projects to s=10.5, t=6.5: taps (10,6),(11,6),(10,7),(11,7).
// The occluder hides exactly the two column-10 taps, so half the bilinear
// weight survives while the receiver itself covers all four tap pixels.
constexpr size_t kOcclusionTexel = 6 + 2 * 8;

int case_foreground_blocks_some_taps() {
    ObjScene scene = make_occluder_scene("occl");
    Model receiver(scene.model_paths[0]), occluder(scene.model_paths[1]);
    ProjectiveGeometry geometry({&receiver, &occluder});
    std::vector<double> img = uniform_image(OccluderScene::W, OccluderScene::H, .2, .4, .6);
    SourceCapture capture(geometry, identity4(), {0., 0., 2.}, img,
                          OccluderScene::W, OccluderScene::H);
    BakeResult blocked = bake_atlas(geometry, {&capture}, 8, 8, 0., nullptr);
    size_t idx = kOcclusionTexel;
    TASK_CHECK(blocked.covered[idx] == 1);
    TASK_CHECK(blocked.owner_model[idx] == 0);
    double lambda[3];
    TASK_CHECK(uv_barycentric(receiver, blocked.owner_face[idx],
                              vec2{.8125, .3125}, lambda));
    vec3 point = reconstruct_point(receiver, blocked.owner_face[idx], lambda);
    vec3 normal = geometric_normal(receiver, blocked.owner_face[idx]);
    double s, t;
    project_point(identity4(), point, s, t, OccluderScene::W, OccluderScene::H);
    TASK_CHECK(near(s, 10.5, 1e-12) && near(t, 6.5, 1e-12));
    TASK_CHECK(capture.has_depth(10, 6) && capture.has_depth(10, 7));
    TASK_CHECK(capture.has_depth(11, 6) && capture.has_depth(11, 7));
    double facing = facing_factor4(point, normal, vec3{0., 0., 2.});
    TASK_CHECK(near(blocked.weight[idx], .5 * facing, 1e-12)); // two of four taps blocked
    TASK_CHECK(near(blocked.rgb[idx * 3], .2, 1e-12) &&
               near(blocked.rgb[idx * 3 + 1], .4, 1e-12) &&
               near(blocked.rgb[idx * 3 + 2], .6, 1e-12));

    // Without the occluder participating in the depth capture all four taps
    // contribute and the total weight goes back to the full facing factor.
    ProjectiveGeometry receiver_only({&receiver});
    SourceCapture clear_capture(receiver_only, identity4(), {0., 0., 2.}, img,
                                OccluderScene::W, OccluderScene::H);
    BakeResult clear = bake_atlas(receiver_only, {&clear_capture}, 8, 8, 0., nullptr);
    TASK_CHECK(near(clear.weight[idx], facing, 1e-12));
    TASK_CHECK(clear.weight[idx] > blocked.weight[idx]);
    return 0;
}

int case_steep_slope_not_self_occluded() {
    using V = ObjScene::V;
    using F = ObjScene::F;
    // A steeply sloped triangle: depth varies strongly across the image.
    // The per-tap extrapolated receiver depth must match the captured depth
    // exactly (same integer-pixel kernel), so eps=0 cannot reject its own taps.
    std::vector<std::vector<V>> verts = {{ {-.5, -.5, 0.}, {.5, -.5, 0.}, {.5, .5, .9} }};
    std::vector<std::vector<V>> uvs   = {{ {0., 0.}, {1., 0.}, {1., 1.} }};
    std::vector<std::vector<F>> faces(1);
    faces[0].push_back(ObjScene::face(0, 1, 2));
    ObjScene scene = ObjScene::create("steep", verts, uvs, faces);
    Model model(scene.model_paths[0]);
    ProjectiveGeometry geometry({&model});
    const int W = 16, H = 16;
    std::vector<double> img = uniform_image(W, H, .11, .22, .33);
    SourceCapture capture(geometry, identity4(), {0., 0., 2.}, img, W, H);
    BakeResult result = bake_atlas(geometry, {&capture}, 8, 8, 0., nullptr);

    int full_weight_texels = 0;
    for (int ty = 1; ty < 7; ++ty) {
        for (int tx = 1; tx < 7; ++tx) {
            size_t idx = static_cast<size_t>(ty) * 8 + tx;
            if (!result.covered[idx]) continue;
            double lambda[3];
            vec2 center{(tx + .5) / 8., (ty + .5) / 8.};
            TASK_CHECK(uv_barycentric(model, result.owner_face[idx], center, lambda));
            vec3 point = reconstruct_point(model, result.owner_face[idx], lambda);
            vec3 normal = geometric_normal(model, result.owner_face[idx]);
            double expected_weight = facing_factor4(point, normal, vec3{0., 0., 2.});
            TASK_CHECK(near(result.weight[idx], expected_weight, 1e-12));
            TASK_CHECK(result.weight[idx] > 0.);
            TASK_CHECK(near(result.rgb[idx * 3], .11, 1e-12));
            ++full_weight_texels;
        }
    }
    TASK_CHECK(full_weight_texels >= 10);
    return 0;
}

int case_culled_source_contributes_nothing() {
    ObjScene scene = make_rect_scene("cull");
    Model model(scene.model_paths[0]);
    ProjectiveGeometry geometry({&model});
    const int W = 16, H = 16;
    std::vector<double> img = uniform_image(W, H, .5, .5, .5);
    SourceCapture normal(geometry, identity4(), {0., 0., 2.}, img, W, H);
    mat<4, 4> mirror = identity4();
    mirror[0][0] = -1.; // flips projected winding: forward area rule culls it
    SourceCapture flipped(geometry, mirror, {0., 0., 2.}, img, W, H);

    BakeResult one = bake_atlas(geometry, {&normal}, 8, 8, 0., nullptr);
    BakeResult two = bake_atlas(geometry, {&normal, &flipped}, 8, 8, 0., nullptr);
    for (size_t i = 0; i < one.rgb.size(); ++i)
        TASK_CHECK(near(one.rgb[i], two.rgb[i]));
    for (size_t i = 0; i < one.weight.size(); ++i)
        TASK_CHECK(near(one.weight[i], two.weight[i]));
    return 0;
}

int case_depth_eps_tolerance() {
    // The occluder sits only 1e-4 in front of the receiver and hides the same
    // two taps as the foreground test.
    ObjScene scene = make_occluder_scene("eps", 1e-4);
    Model receiver(scene.model_paths[0]), occluder(scene.model_paths[1]);
    ProjectiveGeometry geometry({&receiver, &occluder});
    std::vector<double> img = uniform_image(16, 16, .2, .4, .6);
    SourceCapture capture(geometry, identity4(), {0., 0., 2.}, img, 16, 16);

    BakeResult strict = bake_atlas(geometry, {&capture}, 8, 8, 0., nullptr);
    BakeResult relaxed = bake_atlas(geometry, {&capture}, 8, 8, 1e-2, nullptr);
    size_t idx = kOcclusionTexel;
    double lambda[3];
    TASK_CHECK(uv_barycentric(receiver, strict.owner_face[idx],
                              vec2{.8125, .3125}, lambda));
    vec3 point = reconstruct_point(receiver, strict.owner_face[idx], lambda);
    vec3 normal = geometric_normal(receiver, strict.owner_face[idx]);
    double facing = facing_factor4(point, normal, vec3{0., 0., 2.});
    TASK_CHECK(near(strict.weight[idx], .5 * facing, 1e-12));  // two taps still blocked
    TASK_CHECK(near(relaxed.weight[idx], facing, 1e-12));      // tolerance admits them
    return 0;
}

// ---------------------------------------------------------------- lifetime

int case_snapshot_survives_caller_changes() {
    ObjScene scene = make_rect_scene("life");
    auto owned_model = std::make_unique<Model>(scene.model_paths[0]);
    ProjectiveGeometry geometry({owned_model.get()});
    const int W = 16, H = 16;
    std::vector<double> img = uniform_image(W, H, .13, .26, .39);
    SourceCapture capture(geometry, identity4(), {0., 0., 2.}, img, W, H);
    BakeResult before = bake_atlas(geometry, {&capture}, 8, 8, 0., nullptr);

    // Destroy the caller's Model, rewrite the source image in place, scramble
    // the global rasterizer state and interleave an unrelated capture.
    owned_model.reset();
    std::fill(img.begin(), img.end(), 999.);
    lookat({7., -3., 11.}, {1., 2., 3.}, {0., 1., 0.});
    init_perspective(3.5);
    init_viewport(3, 4, 27, 29);
    init_zbuffer(5, 7);
    {
        ObjScene other = make_occluder_scene("life_other");
        Model m0(other.model_paths[0]), m1(other.model_paths[1]);
        ProjectiveGeometry other_geo({&m0, &m1});
        std::vector<double> other_img = uniform_image(8, 8, .01, .02, .03);
        SourceCapture other_capture(other_geo, identity4(), {0., 0., 2.},
                                    other_img, 8, 8);
        bake_atlas(other_geo, {&other_capture}, 4, 4, 0., nullptr);
    }

    BakeResult after = bake_atlas(geometry, {&capture}, 8, 8, 0., nullptr);
    TASK_CHECK(before.rgb == after.rgb);
    TASK_CHECK(before.weight == after.weight);
    TASK_CHECK(before.covered == after.covered);
    TASK_CHECK(before.owner_face == after.owner_face &&
               before.owner_model == after.owner_model);
    return 0;
}

int case_source_order_independent() {
    ObjScene scene = make_rect_scene("order");
    Model model(scene.model_paths[0]);
    ProjectiveGeometry geometry({&model});
    const int W = 16, H = 16;
    std::vector<double> img_a = uniform_image(W, H, .8, .1, .1);
    std::vector<double> img_b = uniform_image(W, H, .1, .8, .1);
    SourceCapture a(geometry, identity4(), {0., 0., 2.}, img_a, W, H);
    SourceCapture b(geometry, identity4(), {0., 0., 2.}, img_b, W, H);

    BakeResult ab = bake_atlas(geometry, {&a, &b}, 8, 8, 0., nullptr);
    BakeResult ba = bake_atlas(geometry, {&b, &a}, 8, 8, 0., nullptr);
    size_t idx = 4 + 4 * 8;
    double lambda[3];
    TASK_CHECK(uv_barycentric(model, ab.owner_face[idx], vec2{.5625, .5625}, lambda));
    vec3 point = reconstruct_point(model, ab.owner_face[idx], lambda);
    vec3 normal = geometric_normal(model, ab.owner_face[idx]);
    double facing = facing_factor4(point, normal, vec3{0., 0., 2.});
    TASK_CHECK(near(ab.weight[idx], 2. * facing, 1e-12) &&
               near(ba.weight[idx], 2. * facing, 1e-12));
    TASK_CHECK(near(ab.rgb[idx * 3], .45, 1e-12) &&
               near(ab.rgb[idx * 3 + 1], .45, 1e-12) &&
               near(ab.rgb[idx * 3 + 2], .1, 1e-12));
    for (size_t i = 0; i < ab.rgb.size(); ++i)
        TASK_CHECK(near(ab.rgb[i], ba.rgb[i], 1e-12));
    return 0;
}

int case_raster_state_restored() {
    ObjScene scene = make_rect_scene("state");
    Model model(scene.model_paths[0]);
    ProjectiveGeometry geometry({&model});
    lookat({1., 2., 3.}, {0., 0., 0.}, {0., 1., 0.});
    init_perspective(2.5);
    init_viewport(1, 2, 13, 17);
    init_zbuffer(6, 9);
    mat<4, 4> saved_mv = ModelView, saved_vp = Viewport, saved_p = Perspective;
    std::vector<double> saved_zb = zbuffer;

    std::vector<double> img = uniform_image(8, 8, .1, .2, .3);
    SourceCapture capture(geometry, identity4(), {0., 0., 2.}, img, 8, 8);
    BakeResult result = bake_atlas(geometry, {&capture}, 5, 5, 0., nullptr);
    TASK_CHECK(result.width == 5);
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) {
            TASK_CHECK(near(ModelView[i][j], saved_mv[i][j]));
            TASK_CHECK(near(Viewport[i][j], saved_vp[i][j]));
            TASK_CHECK(near(Perspective[i][j], saved_p[i][j]));
        }
    TASK_CHECK(zbuffer == saved_zb);
    return 0;
}

int case_input_validation() {
    ObjScene scene = make_rect_scene("valid");
    Model model(scene.model_paths[0]);
    ProjectiveGeometry geometry({&model});
    std::vector<double> ok_img = uniform_image(4, 4, .1, .2, .3);

    bool threw = false;
    try { SourceCapture bad(geometry, identity4(), {0., 0., 2.}, ok_img, 4, 3); }
    catch (const bake_error &) { threw = true; }
    TASK_CHECK(threw);

    threw = false;
    try { SourceCapture bad(geometry, identity4(), {0., 0., 2.}, ok_img, 0, 4); }
    catch (const bake_error &) { threw = true; }
    TASK_CHECK(threw);

    std::vector<double> nan_img = ok_img;
    nan_img[5] = std::nan("");
    threw = false;
    try { SourceCapture bad(geometry, identity4(), {0., 0., 2.}, nan_img, 4, 4); }
    catch (const bake_error &) { threw = true; }
    TASK_CHECK(threw);

    threw = false;
    try { bake_atlas(geometry, {}, 8, -1, 0., nullptr); }
    catch (const bake_error &) { threw = true; }
    TASK_CHECK(threw);

    SourceCapture capture(geometry, identity4(), {0., 0., 2.}, ok_img, 4, 4);
    threw = false;
    try { bake_atlas(geometry, {&capture}, 8, 8, -1e-9, nullptr); }
    catch (const bake_error &) { threw = true; }
    TASK_CHECK(threw);

    // UV outside [0,1] is rejected while building the geometry snapshot.
    using V = ObjScene::V;
    using F = ObjScene::F;
    std::vector<std::vector<V>> verts = {{ {0., 0., 0.}, {1., 0., 0.}, {0., 1., 0.} }};
    std::vector<std::vector<V>> bad_uvs = {{ {0., 0.}, {1.2, 0.}, {0., 1.} }};
    std::vector<std::vector<F>> faces(1);
    faces[0].push_back(ObjScene::face(0, 1, 2));
    ObjScene bad_scene = ObjScene::create("baduv", verts, bad_uvs, faces);
    Model bad_model(bad_scene.model_paths[0]);
    threw = false;
    try { ProjectiveGeometry bad_geo({&bad_model}); }
    catch (const bake_error &) { threw = true; }
    TASK_CHECK(threw);
    return 0;
}

} // namespace

const task_case task_cases[] = {
    {"projection", "ortho_color_and_weight",      case_ortho_color_and_weight},
    {"projection", "perspective_different_w",     case_perspective_different_w},
    {"projection", "mirrored_uv_shared_edge",     case_mirrored_uv_shared_edge},
    {"projection", "overlap_rejected",            case_overlap_rejected},
    {"projection", "empty_sources_keep_coverage", case_empty_sources_keep_coverage},
    {"visibility", "foreground_blocks_some_taps", case_foreground_blocks_some_taps},
    {"visibility", "steep_slope_self_occlusion",  case_steep_slope_not_self_occluded},
    {"visibility", "culled_source_no_contribution", case_culled_source_contributes_nothing},
    {"visibility", "depth_eps_tolerance",         case_depth_eps_tolerance},
    {"lifetime",   "snapshot_survives_changes",   case_snapshot_survives_caller_changes},
    {"lifetime",   "source_order_independent",    case_source_order_independent},
    {"lifetime",   "raster_state_restored",       case_raster_state_restored},
    {"lifetime",   "input_validation",            case_input_validation},
};
const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
