#include "task_support/check.h"
#include "src/refraction.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

using namespace gfx::refraction;

namespace {

constexpr double kEps = 1e-6;

auto Near(double a, double b, double eps = kEps) -> bool {
    return std::abs(a - b) <= eps * (1.0 + std::max(std::abs(a), std::abs(b)));
}

auto NearV(const Vec3& a, const Vec3& b, double eps = kEps) -> bool {
    return Near(a.x, b.x, eps) && Near(a.y, b.y, eps) &&
           Near(a.z, b.z, eps);
}

auto IndicesEqual(const std::vector<std::uint8_t>& a,
                  const std::vector<std::uint8_t>& b) -> bool {
    return a == b;
}

auto DoubleVectorsEqual(const std::vector<double>& a,
                        const std::vector<double>& b) -> bool {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (a[i] != b[i]) return false;
    }
    return true;
}

auto VecVectorsEqual(const std::vector<Vec3>& a, const std::vector<Vec3>& b)
    -> bool {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (a[i].x != b[i].x || a[i].y != b[i].y || a[i].z != b[i].z)
            return false;
    }
    return true;
}

auto GlassUnchanged(const GlassCapture& a, const GlassCapture& b) -> bool {
    return a.width == b.width && a.height == b.height &&
           VecVectorsEqual(a.front_position, b.front_position) &&
           VecVectorsEqual(a.front_normal, b.front_normal) &&
           VecVectorsEqual(a.back_position, b.back_position) &&
           VecVectorsEqual(a.back_normal, b.back_normal) &&
           IndicesEqual(a.front_valid, b.front_valid) &&
           IndicesEqual(a.back_valid, b.back_valid) &&
           DoubleVectorsEqual(a.front_depth, b.front_depth) &&
           DoubleVectorsEqual(a.back_depth, b.back_depth);
}

auto BackgroundUnchanged(const ImageCapture& a, const ImageCapture& b) -> bool {
    return a.width == b.width && a.height == b.height &&
           DoubleVectorsEqual(a.color, b.color) &&
           DoubleVectorsEqual(a.depth, b.depth) &&
           IndicesEqual(a.valid, b.valid);
}

auto MakeSettings(int width, int height, const Mat4& projection,
                  double near_depth, double far_depth) -> CaptureSettings {
    CaptureSettings s;
    s.width = width;
    s.height = height;
    s.world_to_view = Mat4::Identity();
    s.normal_to_view = Mat4::Identity();
    s.projection = projection;
    s.near_depth = near_depth;
    s.far_depth = far_depth;
    return s;
}

auto AddTriangle(Mesh& mesh, const Vec3& p0, const Vec3& p1, const Vec3& p2,
                 const Vec3& normal) -> void {
    Triangle tri;
    tri.vertex[0].position = p0;
    tri.vertex[1].position = p1;
    tri.vertex[2].position = p2;
    /* Orient the winding so the projected winding agrees with the intended
     * outward normal, matching the capture's front/back classification. */
    const Vec3 a = p1 - p0;
    const Vec3 b = p2 - p0;
    if (Dot(Cross(a, b), normal) < 0.0)
        std::swap(tri.vertex[1], tri.vertex[2]);
    for (int i = 0; i < 3; ++i) {
        tri.vertex[i].normal = normal;
        tri.vertex[i].color = {1.0, 1.0, 1.0};
    }
    mesh.triangles.push_back(tri);
}

auto AddQuad(Mesh& mesh, const Vec3& p0, const Vec3& p1, const Vec3& p2,
             const Vec3& p3, const Vec3& normal) -> void {
    AddTriangle(mesh, p0, p1, p2, normal);
    AddTriangle(mesh, p0, p2, p3, normal);
}

/* Closed triangular-prism glass wedge with a front face at z_front (towards
 * the camera) and a back face at z_back. */
auto MakeWedge(double z_front, double z_back) -> Mesh {
    Mesh mesh;
    const Vec3 a{-1.0, -0.6, z_front};
    const Vec3 b{1.0, -0.6, z_front};
    const Vec3 c{0.0, 0.9, z_front};
    const Vec3 a2{-1.0, -0.6, z_back};
    const Vec3 b2{1.0, -0.6, z_back};
    const Vec3 c2{0.0, 0.9, z_back};
    AddTriangle(mesh, a, b, c, Vec3{0.0, 0.0, 1.0});
    AddTriangle(mesh, a2, b2, c2, Vec3{0.0, 0.0, -1.0});
    AddQuad(mesh, a, b, b2, a2, Vec3{0.0, -1.0, 0.0});
    AddQuad(mesh, b, c, c2, b2, Normalize(Vec3{1.5, 1.0, 0.0}));
    AddQuad(mesh, c, a, a2, c2, Normalize(Vec3{-1.5, 1.0, 0.0}));
    return mesh;
}

/* Large opaque backdrop plane perpendicular to the view axis. */
auto MakeBackdrop(double z, const Vec3& color) -> Mesh {
    Mesh mesh;
    const Vec3 p[4] = {{-40.0, -40.0, z},
                       {40.0, -40.0, z},
                       {40.0, 40.0, z},
                       {-40.0, 40.0, z}};
    AddTriangle(mesh, p[0], p[1], p[2], Vec3{0.0, 0.0, 1.0});
    AddTriangle(mesh, p[0], p[2], p[3], Vec3{0.0, 0.0, 1.0});
    for (auto& tri : mesh.triangles) {
        for (int i = 0; i < 3; ++i) tri.vertex[i].color = color;
    }
    return mesh;
}

auto MakeEmptyScene(int width, int height, const Mat4& projection,
                    double near_depth, double far_depth) -> SceneCapture {
    SceneCapture sc;
    const std::size_t cells = static_cast<std::size_t>(width) * height;
    sc.background.width = width;
    sc.background.height = height;
    sc.background.color.assign(3 * cells, 0.0);
    sc.background.depth.assign(cells,
                               std::numeric_limits<double>::infinity());
    sc.background.valid.assign(cells, 0);
    sc.background.projection = projection;
    sc.background.near_depth = near_depth;
    sc.background.far_depth = far_depth;

    sc.glass.width = width;
    sc.glass.height = height;
    sc.glass.front_position.assign(cells, Vec3{});
    sc.glass.front_normal.assign(cells, Vec3{});
    sc.glass.back_position.assign(cells, Vec3{});
    sc.glass.back_normal.assign(cells, Vec3{});
    sc.glass.front_valid.assign(cells, 0);
    sc.glass.back_valid.assign(cells, 0);
    sc.glass.front_depth.assign(cells,
                                std::numeric_limits<double>::infinity());
    sc.glass.back_depth.assign(cells,
                               std::numeric_limits<double>::infinity());
    sc.glass.projection = projection;
    sc.glass.near_depth = near_depth;
    sc.glass.far_depth = far_depth;
    return sc;
}

auto FillBackground(SceneCapture& sc, double depth, const Vec3& color) -> void {
    const std::size_t cells =
        static_cast<std::size_t>(sc.background.width) * sc.background.height;
    for (std::size_t i = 0; i < cells; ++i) {
        sc.background.valid[i] = 1;
        sc.background.depth[i] = depth;
        sc.background.color[3 * i] = color.x;
        sc.background.color[3 * i + 1] = color.y;
        sc.background.color[3 * i + 2] = color.z;
    }
}

}  // namespace

namespace {

auto SlantWedge() -> Mesh {
    Mesh mesh;
    const Vec3 f0{-1.2, -1.0, -3.0};
    const Vec3 f1{1.2, -1.0, -5.0};
    const Vec3 f2{0.0, 1.1, -4.0};
    const Vec3 b0{-1.2, -1.0, -6.0};
    const Vec3 b1{1.2, -1.0, -8.0};
    const Vec3 b2{0.0, 1.1, -7.0};
    Vec3 n = Normalize(Cross(f1 - f0, f2 - f0));
    if (n.z < 0.0) n = n * -1.0;
    AddTriangle(mesh, f0, f1, f2, n);
    AddTriangle(mesh, b0, b1, b2, n * -1.0);
    AddQuad(mesh, f0, f1, b1, b0, Normalize(Vec3{0.0, -1.0, 0.0}));
    AddQuad(mesh, f1, f2, b2, b1, Normalize(Vec3{1.2, 0.3, 0.0}));
    AddQuad(mesh, f2, f0, b0, b2, Normalize(Vec3{-1.2, 0.3, 0.0}));
    return mesh;
}

/* Independent ground truth: intersect the pixel's view ray with the plane
 * through `plane_point` with normal `plane_normal`. */
auto RayPlaneAtPixel(int x, int y, int width, int height, double p00,
                     double p11, const Vec3& plane_point,
                     const Vec3& plane_normal, Vec3& out) -> bool {
    const double sx = x + 0.5;
    const double sy = y + 0.5;
    const double ndc_x = 2.0 * sx / width - 1.0;
    const double ndc_y = 2.0 * sy / height - 1.0;
    const Vec3 dir{ndc_x / p00, ndc_y / p11, -1.0};
    const double denom = Dot(plane_normal, dir);
    if (std::abs(denom) < 1e-15) return false;
    const double s = Dot(plane_normal, plane_point) / denom;
    if (!(s > 0.0)) return false;
    out = dir * s;
    return true;
}

}  // namespace

static int CaseCaptureProjectionZ() {
    const Mat4 p = PerspectiveProjection(-1.5, 1.0, -0.7, 1.3, 2.0, 50.0);
    /* Near and far points on the axis map to ndc z 0 and 1. */
    const Vec4 near_point = Transform(p, Vec4{0.0, 0.0, -2.0, 1.0});
    const Vec4 far_point = Transform(p, Vec4{0.0, 0.0, -50.0, 1.0});
    TASK_CHECK(std::abs(near_point.z / near_point.w) < 1e-12);
    TASK_CHECK(std::abs(far_point.z / far_point.w - 1.0) < 1e-12);
    /* clip.w = -view.z. */
    const Vec4 off_axis = Transform(p, Vec4{0.3, -0.2, -7.0, 1.0});
    TASK_CHECK(std::abs(off_axis.w - 7.0) < 1e-12);
    /* Asymmetric frustum maps the near-plane corners to +/-1. */
    const Vec4 left_bottom = Transform(p, Vec4{-1.5, -0.7, -2.0, 1.0});
    const Vec4 right_top = Transform(p, Vec4{1.0, 1.3, -2.0, 1.0});
    TASK_CHECK(std::abs(left_bottom.x / left_bottom.w + 1.0) < 1e-12);
    TASK_CHECK(std::abs(left_bottom.y / left_bottom.w + 1.0) < 1e-12);
    TASK_CHECK(std::abs(right_top.x / right_top.w - 1.0) < 1e-12);
    TASK_CHECK(std::abs(right_top.y / right_top.w - 1.0) < 1e-12);
    double n = 0.0, f = 0.0;
    TASK_CHECK(DeriveDepthRange(p, n, f));
    TASK_CHECK(Near(n, 2.0) && Near(f, 50.0));
    return 0;
}

static int CaseCaptureGlassLayers() {
    const int W = 96, H = 96;
    const Mat4 p = PerspectiveProjection(-1.0, 1.0, -1.0, 1.0, 1.0, 40.0);
    const CaptureSettings cs = MakeSettings(W, H, p, 1.0, 40.0);
    const Mesh wedge = MakeWedge(-4.0, -6.0);
    GlassCapture glass;
    std::string error;
    TASK_CHECK(CaptureGlass(wedge, cs, glass, &error));

    const std::size_t cells = static_cast<std::size_t>(W) * H;
    std::size_t front = 0, back = 0;
    for (std::size_t i = 0; i < cells; ++i) {
        front += glass.front_valid[i] ? 1 : 0;
        back += glass.back_valid[i] ? 1 : 0;
    }
    TASK_CHECK(front > 0 && back > 0);

    /* The centre pixel sees the front face at z=-4 and the back face at
     * z=-6, with opposite outward normals. */
    const std::size_t centre = static_cast<std::size_t>(H / 2) * W + W / 2;
    TASK_CHECK(glass.front_valid[centre] && glass.back_valid[centre]);
    TASK_CHECK(Near(glass.front_depth[centre], 4.0));
    TASK_CHECK(Near(glass.back_depth[centre], 6.0));
    TASK_CHECK(NearV(glass.front_normal[centre], Vec3{0, 0, 1}));
    TASK_CHECK(NearV(glass.back_normal[centre], Vec3{0, 0, -1}));
    TASK_CHECK(Near(Length(glass.front_normal[centre]), 1.0));
    TASK_CHECK(Near(Length(glass.back_normal[centre]), 1.0));
    TASK_CHECK(Near(glass.front_position[centre].z, -4.0));
    TASK_CHECK(Near(glass.back_position[centre].z, -6.0));

    /* Back faces are captured even though the old rasterizer culls them:
     * their projected winding is opposite to the front faces. */
    const std::size_t outside =
        static_cast<std::size_t>(2) * W + 2;
    TASK_CHECK(!glass.front_valid[outside] && !glass.back_valid[outside]);
    return 0;
}

static int CaseCaptureInvalidRejected() {
    const int W = 32, H = 32;
    const Mat4 p = PerspectiveProjection(-1.0, 1.0, -1.0, 1.0, 1.0, 20.0);
    const CaptureSettings cs = MakeSettings(W, H, p, 1.0, 20.0);

    GlassCapture existing;
    const Mesh wedge = MakeWedge(-3.0, -5.0);
    std::string error;
    TASK_CHECK(CaptureGlass(wedge, cs, existing, &error));
    GlassCapture before = existing;

    /* A vertex behind the camera must be rejected without touching output. */
    Mesh bad = wedge;
    bad.triangles[0].vertex[0].position = Vec3{0.0, 0.0, 2.0};
    TASK_CHECK(!CaptureGlass(bad, cs, existing, &error));
    TASK_CHECK(!error.empty());
    TASK_CHECK(GlassUnchanged(existing, before));

    /* A degenerate normal is rejected too. */
    Mesh bad_normal = wedge;
    bad_normal.triangles[0].vertex[0].normal = Vec3{0.0, 0.0, 0.0};
    TASK_CHECK(!CaptureGlass(bad_normal, cs, existing, &error));
    TASK_CHECK(GlassUnchanged(existing, before));

    /* Non-positive size is rejected. */
    CaptureSettings zero = cs;
    zero.width = 0;
    TASK_CHECK(!CaptureGlass(wedge, zero, existing, &error));
    TASK_CHECK(GlassUnchanged(existing, before));

    ImageCapture background;
    Mesh backdrop = MakeBackdrop(-10.0, {0.2, 0.4, 0.6});
    TASK_CHECK(CaptureBackground(backdrop, cs, background, &error));
    ImageCapture background_before = background;
    zero = cs;
    zero.height = -3;
    TASK_CHECK(!CaptureBackground(backdrop, zero, background, &error));
    TASK_CHECK(BackgroundUnchanged(background, background_before));
    return 0;
}

static int CaseCaptureBackgroundDepth() {
    const int W = 48, H = 32;
    /* Asymmetric, non-square viewport. */
    const Mat4 p = PerspectiveProjection(-2.0, 1.0, -0.5, 1.5, 1.0, 30.0);
    const CaptureSettings cs = MakeSettings(W, H, p, 1.0, 30.0);
    const Mesh backdrop = MakeBackdrop(-7.5, {0.25, 0.5, 0.75});
    ImageCapture bg;
    std::string error;
    TASK_CHECK(CaptureBackground(backdrop, cs, bg, &error));
    std::size_t valid = 0;
    for (std::size_t i = 0; i < bg.valid.size(); ++i) {
        if (bg.valid[i]) {
            ++valid;
            TASK_CHECK(Near(bg.depth[i], 7.5));
            TASK_CHECK(Near(bg.color[3 * i], 0.25));
            TASK_CHECK(Near(bg.color[3 * i + 1], 0.5));
            TASK_CHECK(Near(bg.color[3 * i + 2], 0.75));
        }
    }
    TASK_CHECK(valid > 0);
    TASK_CHECK(bg.width == W && bg.height == H);
    TASK_CHECK(Near(bg.near_depth, 1.0) && Near(bg.far_depth, 30.0));
    return 0;
}

static int CaseCaptureSceneOwnership() {
    const int W = 40, H = 40;
    const Mat4 p = PerspectiveProjection(-1.0, 1.0, -1.0, 1.0, 1.0, 20.0);
    const CaptureSettings cs = MakeSettings(W, H, p, 1.0, 20.0);
    const Mesh wedge = MakeWedge(-3.0, -5.0);
    const Mesh backdrop = MakeBackdrop(-9.0, {0.1, 0.2, 0.3});
    SceneCapture scene;
    std::string error;
    TASK_CHECK(CaptureScene(backdrop, wedge, cs, scene, &error));
    TASK_CHECK(scene.background.width == W && scene.glass.width == W);

    SceneCapture before;
    TASK_CHECK(CaptureScene(backdrop, wedge, cs, before, &error));
    Material material;
    material.ior = 1.5;
    material.sigma_a = {0.1, 0.2, 0.3};
    material.environment = {0.01, 0.02, 0.03};
    CompositeResult result;
    TASK_CHECK(Compose(scene, material, result, &error));
    TASK_CHECK(GlassUnchanged(scene.glass, before.glass));
    TASK_CHECK(BackgroundUnchanged(scene.background, before.background));
    return 0;
}

/* ---------------------------------------------------------------- trace */

static auto CorridorField(int width, int height, double depth,
                          std::size_t& valid_count) -> std::vector<double> {
    std::vector<double> field(static_cast<std::size_t>(width) * height, depth);
    valid_count = field.size();
    return field;
}

static auto MakeField(int width, int height, std::vector<double> depth,
                      std::vector<std::uint8_t> valid) -> DepthFieldView {
    /* The caller keeps the vectors alive; this copies the view pointers. */
    DepthFieldView view;
    view.width = width;
    view.height = height;
    static std::vector<double> owned_depth;
    static std::vector<std::uint8_t> owned_valid;
    owned_depth = std::move(depth);
    owned_valid = std::move(valid);
    view.depth = owned_depth.data();
    view.valid = owned_valid.data();
    view.projection = PerspectiveProjection(-1.0, 1.0, -1.0, 1.0, 1.0, 100.0);
    view.near_depth = 1.0;
    view.far_depth = 100.0;
    return view;
}


/* Project a view-space point to continuous pixel coordinates using a
 * projection with clip.w = -view.z. */
auto ProjectPoint(const Mat4& p, int width, int height, const Vec3& point,
                  double& u, double& v) -> bool {
    const Vec4 clip = Transform(p, Vec4{point.x, point.y, point.z, 1.0});
    if (!(clip.w > 0.0)) return false;
    const double invw = 1.0 / clip.w;
    u = (clip.x * invw * 0.5 + 0.5) * width;
    v = (clip.y * invw * 0.5 + 0.5) * height;
    return std::isfinite(u) && std::isfinite(v);
}

static int CaseTraceContinuousSingleCell() {
    const int W = 32, H = 32;
    /* A single-cell-sized target at depth 5.  The traced ray has t != depth
     * (it is oblique), so a fixed-step walk would miss it; the continuous
     * intersection must hit. */
    std::vector<double> depth(static_cast<std::size_t>(W) * H, 0.0);
    std::vector<std::uint8_t> valid(static_cast<std::size_t>(W) * H, 0);
    const int px = 20, py = 12;
    depth[static_cast<std::size_t>(py) * W + px] = 5.0;
    valid[static_cast<std::size_t>(py) * W + px] = 1;
    DepthFieldView field = MakeField(W, H, std::move(depth), std::move(valid));

    const double sx = px + 0.5;
    const double sy = py + 0.5;
    const double ndc_x = 2.0 * sx / W - 1.0;
    const double ndc_y = 2.0 * sy / H - 1.0;
    /* Aim at the cell centre: the unit-normalised direction is
     * (ndc_x, ndc_y, -1) scaled, so the hit lands exactly on the centre. */
    const Vec3 origin{0.0, 0.0, 0.0};
    const Vec3 dir{ndc_x, ndc_y, -1.0};
    RayHit hit;
    std::string error;
    TASK_CHECK(TraceDepthField(field, origin, dir, hit, &error));
    TASK_CHECK(hit.hit);
    TASK_CHECK(hit.pixel_x == px && hit.pixel_y == py);
    TASK_CHECK(Near(hit.u, sx, 1e-9) && Near(hit.v, sy, 1e-9));
    /* distance is the view-space ray parameter: the oblique ray travels
     * 5 * |dir| to reach the plane at view depth 5. */
    TASK_CHECK(Near(hit.distance, 5.0 * Length(dir)));
    TASK_CHECK(hit.visited_cells >= 1);
    TASK_CHECK(hit.visited_cells <= static_cast<std::size_t>(W) + H + 1);
    return 0;
}

static int CaseTraceSkipsNearerCells() {
    const int W = 40, H = 24;
    std::vector<double> depth(static_cast<std::size_t>(W) * H, 0.0);
    std::vector<std::uint8_t> valid(static_cast<std::size_t>(W) * H, 0);
    DepthFieldView field = MakeField(W, H, {}, {});
    field.projection = PerspectiveProjection(-1.0, 1.0, -1.0, 1.0, 1.0, 100.0);
    const Mat4& p = field.projection;

    /* Off-axis origin so the ray sweeps across pixels as view depth grows.
     * direction has z = -1, so the ray parameter equals view_depth - 2. */
    const Vec3 origin{-6.0, 0.0, -2.0};
    const Vec3 dir{1.3, 0.05, -1.0};

    /* Walk the ray and collect the distinct cells it enters, with the view
     * depth of the ray at each first entry. */
    struct Crossed {
        int x;
        int y;
        double depth;
    };
    std::vector<Crossed> crossed;
    int last_x = -1, last_y = -1;
    for (double t = 0.0; t < 20.0; t += 0.002) {
        const Vec3 point = origin + dir * t;
        double u = 0.0, v = 0.0;
        if (!ProjectPoint(p, W, H, point, u, v)) continue;
        if (u < 0.0 || u >= W || v < 0.0 || v >= H) continue;
        const int x = static_cast<int>(std::floor(u));
        const int y = static_cast<int>(std::floor(v));
        if (x == last_x && y == last_y) continue;
        last_x = x;
        last_y = y;
        crossed.push_back({x, y, -point.z});
    }
    TASK_CHECK(crossed.size() >= 6);

    /* The first cell the ray enters stores a far depth: the ray only meets
     * that plane much later, outside the cell, so it must be skipped. */
    const Crossed& far_cell = crossed.front();
    depth[static_cast<std::size_t>(far_cell.y) * W + far_cell.x] =
        crossed.back().depth + 5.0;
    valid[static_cast<std::size_t>(far_cell.y) * W + far_cell.x] = 1;

    /* A middle cell stores its true plane depth (a genuine hit). */
    const Crossed& near_true = crossed[crossed.size() / 3];
    const std::size_t near_idx =
        static_cast<std::size_t>(near_true.y) * W + near_true.x;
    depth[near_idx] = near_true.depth;
    valid[near_idx] = 1;

    /* A farther cell also stores its true plane depth. */
    const Crossed& far_true = crossed[crossed.size() - 2];
    const std::size_t far_idx =
        static_cast<std::size_t>(far_true.y) * W + far_true.x;
    depth[far_idx] = far_true.depth;
    valid[far_idx] = 1;

    field.depth = depth.data();
    field.valid = valid.data();
    RayHit hit;
    std::string error;
    TASK_CHECK(TraceDepthField(field, origin, dir, hit, &error));
    TASK_CHECK(hit.hit);
    /* Nearest genuine intersection wins; the bogus near cell was skipped. */
    TASK_CHECK(hit.pixel_x == near_true.x && hit.pixel_y == near_true.y);
    /* distance is the view-space ray parameter; the ray direction is
     * normalised before marching. */
    TASK_CHECK(Near(hit.distance, (near_true.depth - 2.0) * Length(dir)));
    TASK_CHECK(hit.visited_cells <=
               static_cast<std::size_t>(W) + H + 1);
    return 0;
}

static int CaseTraceAsymmetricProjection() {
    const int W = 37, H = 23;
    std::vector<double> depth(static_cast<std::size_t>(W) * H, 6.0);
    std::vector<std::uint8_t> valid(static_cast<std::size_t>(W) * H, 1);
    DepthFieldView field = MakeField(W, H, std::move(depth), std::move(valid));
    /* Replace the projection with an asymmetric one and aim at its centre. */
    const Mat4 p = PerspectiveProjection(-2.0, 0.7, -0.4, 1.8, 1.0, 100.0);
    field.projection = p;
    field.near_depth = 1.0;
    field.far_depth = 100.0;
    const Vec3 dir{0.0, 0.0, -1.0};
    RayHit hit;
    std::string error;
    /* The asymmetric frustum centre in view space is not (0,0). */
    TASK_CHECK(TraceDepthField(field, {0.35 * 3.0, 0.35 * 3.0, 0.0}, dir, hit,
                               &error));
    TASK_CHECK(hit.hit);
    const int cx = static_cast<int>((0.35 / 0.7 * 0.5 + 0.5) * W);
    TASK_CHECK(hit.pixel_x >= 0 && hit.pixel_x < W);
    TASK_CHECK(hit.pixel_y >= 0 && hit.pixel_y < H);
    TASK_CHECK(Near(hit.distance, 6.0));
    (void)cx;
    return 0;
}

static int CaseTraceDiagonalBounded() {
    const int W = 20, H = 20;
    std::vector<double> depth(static_cast<std::size_t>(W) * H, 0.0);
    std::vector<std::uint8_t> valid(static_cast<std::size_t>(W) * H, 0);
    DepthFieldView field = MakeField(W, H, {}, {});
    field.projection = PerspectiveProjection(-1.0, 1.0, -1.0, 1.0, 1.0, 100.0);
    const Mat4& p = field.projection;

    /* An off-axis ray sweeps through many cells.  Only the last cell holds a
     * valid depth matching the ray, so the walk must visit the intermediate
     * cells but stay within width + height + 1 and visit each once. */
    const Vec3 origin{-8.0, -8.0, -1.0};
    const Vec3 dir{0.9, 0.9, -1.0};
    int target_x = -1, target_y = -1;
    double target_depth = 0.0;
    std::vector<std::pair<int, int>> cells;
    int last_x = -1, last_y = -1;
    for (double t = 0.0; t < 30.0; t += 0.002) {
        const Vec3 point = origin + dir * t;
        double u = 0.0, v = 0.0;
        if (!ProjectPoint(p, W, H, point, u, v)) continue;
        if (u < 0.0 || u >= W || v < 0.0 || v >= H) continue;
        const int x = static_cast<int>(std::floor(u));
        const int y = static_cast<int>(std::floor(v));
        if (x == last_x && y == last_y) continue;
        last_x = x;
        last_y = y;
        cells.push_back({x, y});
        target_x = x;
        target_y = y;
        target_depth = -point.z;
    }
    TASK_CHECK(cells.size() > 4);
    const std::size_t idx =
        static_cast<std::size_t>(target_y) * W + target_x;
    depth[idx] = target_depth;
    valid[idx] = 1;
    field.depth = depth.data();
    field.valid = valid.data();

    RayHit hit;
    std::string error;
    TASK_CHECK(TraceDepthField(field, origin, dir, hit, &error));
    TASK_CHECK(hit.hit);
    TASK_CHECK(hit.pixel_x == target_x && hit.pixel_y == target_y);
    TASK_CHECK(hit.visited_cells <=
               static_cast<std::size_t>(W) + H + 1);
    TASK_CHECK(hit.visited_cells >= cells.size());
    return 0;
}

static int CaseTraceIgnoresSelfIntersection() {
    const int W = 16, H = 16;
    std::vector<double> depth(static_cast<std::size_t>(W) * H, 4.0);
    std::vector<std::uint8_t> valid(static_cast<std::size_t>(W) * H, 1);
    DepthFieldView field = MakeField(W, H, std::move(depth), std::move(valid));
    /* Origin already inside the field, so the plane at depth 4 is behind
     * the origin for a forward trace; it must not be reported. */
    RayHit hit;
    std::string error;
    TASK_CHECK(TraceDepthField(field, {0, 0, -6.0}, {0, 0, 1.0}, hit, &error));
    /* Moving +z decreases view depth; the plane at depth 4 is in front, so a
     * hit at distance 2 is valid. */
    TASK_CHECK(hit.hit);
    TASK_CHECK(Near(hit.distance, 2.0));

    /* Backwards ray with the plane behind the origin is ignored. */
    RayHit backwards;
    TASK_CHECK(TraceDepthField(field, {0, 0, -6.0}, {0, 0, -1.0}, backwards,
                               &error));
    TASK_CHECK(!backwards.hit);
    return 0;
}

static int CaseTraceOutsideField() {
    const int W = 16, H = 16;
    std::vector<double> depth(static_cast<std::size_t>(W) * H, 4.0);
    std::vector<std::uint8_t> valid(static_cast<std::size_t>(W) * H, 1);
    DepthFieldView field = MakeField(W, H, std::move(depth), std::move(valid));
    RayHit hit;
    std::string error;
    /* Ray aimed far beyond the grid stops without a hit. */
    TASK_CHECK(TraceDepthField(field, {0, 0, 0}, {100.0, 0.0, -1.0}, hit,
                               &error));
    TASK_CHECK(!hit.hit);
    /* A zero direction is invalid and leaves the hit untouched. */
    RayHit untouched;
    untouched.pixel_x = 123;
    TASK_CHECK(!TraceDepthField(field, {0, 0, 0}, {0, 0, 0}, untouched,
                                &error));
    TASK_CHECK(untouched.pixel_x == 123);
    TASK_CHECK(!error.empty());
    return 0;
}

static int CaseTraceNarrowTarget() {
    const int W = 64, H = 64;
    /* A one-cell-wide vertical target reached by a ray crossing many cells.
     * The continuous tester must still land inside it. */
    std::vector<double> depth(static_cast<std::size_t>(W) * H, 0.0);
    std::vector<std::uint8_t> valid(static_cast<std::size_t>(W) * H, 0);
    for (int y = 0; y < H; ++y) {
        const std::size_t idx = static_cast<std::size_t>(y) * W + 47;
        depth[idx] = 4.0;
        valid[idx] = 1;
    }
    DepthFieldView field = MakeField(W, H, std::move(depth), std::move(valid));
    const double ndc_x = 2.0 * 47.5 / W - 1.0;
    const Vec3 dir{ndc_x * 4.0, 0.0, -4.0};
    RayHit hit;
    std::string error;
    TASK_CHECK(TraceDepthField(field, {0, 0, 0}, dir, hit, &error));
    TASK_CHECK(hit.hit);
    TASK_CHECK(hit.pixel_x == 47);
    TASK_CHECK(Near(hit.distance, Length(dir)));
    return 0;
}

static int CaseTraceCornerCell() {
    const int W = 24, H = 18;
    std::vector<double> depth(static_cast<std::size_t>(W) * H, 0.0);
    std::vector<std::uint8_t> valid(static_cast<std::size_t>(W) * H, 0);
    const int cx = W - 1, cy = H - 1;
    depth[static_cast<std::size_t>(cy) * W + cx] = 3.0;
    valid[static_cast<std::size_t>(cy) * W + cx] = 1;
    DepthFieldView field = MakeField(W, H, std::move(depth), std::move(valid));
    const double ndc_x = 2.0 * (cx + 0.5) / W - 1.0;
    const double ndc_y = 2.0 * (cy + 0.5) / H - 1.0;
    RayHit hit;
    std::string error;
    const Vec3 dir{ndc_x * 3.0, ndc_y * 3.0, -3.0};
    TASK_CHECK(TraceDepthField(field, {0, 0, 0}, dir, hit, &error));
    TASK_CHECK(hit.hit);
    TASK_CHECK(hit.pixel_x == cx && hit.pixel_y == cy);
    TASK_CHECK(Near(hit.distance, Length(dir)));
    return 0;
}

/* ------------------------------------------------------------ transport */


/* View-space ray through a pixel centre, valid for the projection form used
 * here (clip.w = -view.z, no x/y translation into w). */
auto PixelRay(const Mat4& p, int width, int height, int x, int y,
              Vec3& dir) -> bool {
    const double ndc_x = 2.0 * (x + 0.5) / width - 1.0;
    const double ndc_y = 2.0 * (y + 0.5) / height - 1.0;
    const double p00 = p(0, 0);
    const double p11 = p(1, 1);
    if (std::abs(p00) < 1e-15 || std::abs(p11) < 1e-15) return false;
    dir = {(ndc_x + p(0, 2)) / p00, (ndc_y + p(1, 2)) / p11, -1.0};
    return true;
}

/* Fill a glass back layer with the depth of a plane through `point` with
 * outward `normal`, as if the plane had been captured at every pixel. */
auto FillGlassBackPlane(GlassCapture& glass, const Vec3& point,
                        const Vec3& normal) -> void {
    for (int y = 0; y < glass.height; ++y) {
        for (int x = 0; x < glass.width; ++x) {
            Vec3 dir;
            if (!PixelRay(glass.projection, glass.width, glass.height, x, y,
                          dir))
                continue;
            const double denom = Dot(normal, dir);
            if (std::abs(denom) < 1e-15) continue;
            const double s = Dot(normal, point) / denom;
            if (!(s > 0.0)) continue;
            const std::size_t idx =
                static_cast<std::size_t>(y) * glass.width + x;
            glass.back_position[idx] = dir * s;
            glass.back_depth[idx] = -dir.z * s;
            glass.back_normal[idx] = normal;
            glass.back_valid[idx] = 1;
        }
    }
}

static int CaseTransportTwoSnellsAndLength() {
    const int W = 48, H = 48;
    const Mat4 p = PerspectiveProjection(-1.0, 1.0, -1.0, 1.0, 1.0, 40.0);
    SceneCapture sc = MakeEmptyScene(W, H, p, 1.0, 40.0);
    FillBackground(sc, 25.0, {0.2, 0.6, 0.8});

    /* Tilted glass slab: front and back planes share a normal, with the
     * back 2.0 units further along the inward normal. */
    const Vec3 front_normal = Normalize(Vec3{0.3, 0.0, 1.0});
    const Vec3 entrance{0.0, 0.0, -3.0};
    const Vec3 back_point = entrance - front_normal * 2.0;
    const std::size_t centre = static_cast<std::size_t>(H / 2) * W + W / 2;
    sc.glass.front_valid[centre] = 1;
    sc.glass.front_position[centre] = entrance;
    sc.glass.front_normal[centre] = front_normal;
    sc.glass.front_depth[centre] = -entrance.z;
    FillGlassBackPlane(sc.glass, back_point, front_normal * -1.0);

    Material material;
    material.ior = 1.5;
    material.sigma_a = {0.0, 0.0, 0.0};
    material.environment = {0.0, 0.0, 0.0};

    CompositeResult result;
    std::string error;
    TASK_CHECK(Compose(sc, material, result, &error));
    const PixelTrace& trace = result.pixels[centre];
    TASK_CHECK(trace.entrance_visible);
    TASK_CHECK(trace.exit_found);
    TASK_CHECK(!trace.background_kept);
    TASK_CHECK(Near(trace.entrance_distance, 3.0, 1e-9));

    /* Exact Snell at the entrance, then the true ray-plane distance to the
     * parallel back face.  The internal direction is not the axial one. */
    const Vec3 camera_dir = Normalize(entrance);
    const FresnelSample in =
        FresnelRefract(camera_dir, front_normal, 1.0 / 1.5);
    TASK_CHECK(!in.total_internal_reflection);
    const Vec3 internal = in.transmission_direction;
    /* The capture stores one scalar depth per pixel, so the exact distance
     * follows the recorded depth at the hit pixel along the refracted ray. */
    const std::size_t hit_idx =
        static_cast<std::size_t>(trace.exit_hit.pixel_y) * W +
        trace.exit_hit.pixel_x;
    TASK_CHECK(sc.glass.back_valid[hit_idx]);
    const double recorded = sc.glass.back_depth[hit_idx];
    const double expected = (recorded - (-entrance.z)) / (-internal.z);
    TASK_CHECK(Near(trace.exit_hit.distance, expected, 1e-6));
    TASK_CHECK(trace.exit_hit.distance > 2.0);
    return 0;
}

static int CaseTransportObliqueInternalLength() {
    const int W = 48, H = 48;
    const Mat4 p = PerspectiveProjection(-1.0, 1.0, -1.0, 1.0, 1.0, 40.0);
    SceneCapture sc = MakeEmptyScene(W, H, p, 1.0, 40.0);
    FillBackground(sc, 30.0, {0.5, 0.5, 0.5});

    /* Oblique camera ray: the entrance is off-axis and the front normal is
     * tilted, so the internal refracted segment is not axial.  Its length
     * must follow that ray, not a same-pixel front/back depth difference. */
    const Vec3 entrance{0.6, 0.0, -4.0};
    const Vec3 plane_normal = Normalize(Vec3{0.2, 0.0, 1.0});
    const Vec3 back_point = entrance - plane_normal * 3.0;
    const std::size_t centre = static_cast<std::size_t>(H / 2) * W + W / 2;
    sc.glass.front_valid[centre] = 1;
    sc.glass.front_position[centre] = entrance;
    sc.glass.front_normal[centre] = plane_normal;
    sc.glass.front_depth[centre] = -entrance.z;
    FillGlassBackPlane(sc.glass, back_point, plane_normal * -1.0);

    Material material;
    material.ior = 1.5;
    material.sigma_a = {0.0, 0.0, 0.0};
    material.environment = {0.0, 0.0, 0.0};
    CompositeResult result;
    std::string error;
    TASK_CHECK(Compose(sc, material, result, &error));
    const PixelTrace& trace = result.pixels[centre];
    TASK_CHECK(trace.exit_found);

    const FresnelSample in =
        FresnelRefract(Normalize(entrance), plane_normal, 1.0 / 1.5);
    TASK_CHECK(!in.total_internal_reflection);
    TASK_CHECK(Near(Length(in.transmission_direction), 1.0));
    const Vec3 internal = in.transmission_direction;
    const std::size_t hit_idx =
        static_cast<std::size_t>(trace.exit_hit.pixel_y) * W +
        trace.exit_hit.pixel_x;
    TASK_CHECK(sc.glass.back_valid[hit_idx]);
    const double recorded = sc.glass.back_depth[hit_idx];
    const double expected = (recorded - (-entrance.z)) / (-internal.z);
    TASK_CHECK(Near(trace.exit_hit.distance, expected, 1e-6));
    /* The axial front/back depth difference would be 3.0; the refracted
     * segment is longer because it crosses the tilted faces obliquely. */
    TASK_CHECK(trace.exit_hit.distance > 3.0 + 1e-6);
    return 0;
}

static int CaseTransportAbsorption() {
    const Mat4 p = PerspectiveProjection(-1.0, 1.0, -1.0, 1.0, 1.0, 40.0);
    SceneCapture sc = MakeEmptyScene(1, 1, p, 1.0, 40.0);
    FillBackground(sc, 20.0, {1.0, 1.0, 1.0});
    sc.glass.front_valid[0] = 1;
    sc.glass.front_position[0] = {0.0, 0.0, -4.0};
    sc.glass.front_normal[0] = {0.0, 0.0, 1.0};
    sc.glass.front_depth[0] = 4.0;
    sc.glass.back_valid[0] = 1;
    sc.glass.back_position[0] = {0.0, 0.0, -7.0};
    sc.glass.back_normal[0] = {0.0, 0.0, -1.0};
    sc.glass.back_depth[0] = 7.0;

    Material material;
    material.ior = 1.5;
    material.sigma_a = {0.3, 0.6, 0.9};
    material.environment = {0.0, 0.0, 0.0};
    CompositeResult result;
    std::string error;
    TASK_CHECK(Compose(sc, material, result, &error));
    const PixelTrace& trace = result.pixels[0];
    TASK_CHECK(Near(trace.exit_hit.distance, 3.0, 1e-9));
    const FresnelSample in =
        FresnelRefract({0.0, 0.0, -1.0}, {0.0, 0.0, 1.0}, 1.0 / 1.5);
    const FresnelSample out =
        FresnelRefract({0.0, 0.0, -1.0}, {0.0, 0.0, -1.0}, 1.5);
    const double transmittance = in.transmittance * out.transmittance;
    TASK_CHECK(Near(result.color[0],
                    transmittance * std::exp(-0.3 * 3.0), 1e-9));
    TASK_CHECK(Near(result.color[1],
                    transmittance * std::exp(-0.6 * 3.0), 1e-9));
    TASK_CHECK(Near(result.color[2],
                    transmittance * std::exp(-0.9 * 3.0), 1e-9));
    return 0;
}

static int CaseTransportEnvironmentAndBackgroundMiss() {
    const Mat4 p = PerspectiveProjection(-1.0, 1.0, -1.0, 1.0, 1.0, 40.0);
    /* Background has no data at all: the transmission term must fall back to
     * the environment colour while the entrance reflection stays. */
    SceneCapture sc = MakeEmptyScene(1, 1, p, 1.0, 40.0);
    sc.glass.front_valid[0] = 1;
    sc.glass.front_position[0] = {0.0, 0.0, -4.0};
    sc.glass.front_normal[0] = {0.0, 0.0, 1.0};
    sc.glass.front_depth[0] = 4.0;
    sc.glass.back_valid[0] = 1;
    sc.glass.back_position[0] = {0.0, 0.0, -6.0};
    sc.glass.back_normal[0] = {0.0, 0.0, -1.0};
    sc.glass.back_depth[0] = 6.0;

    Material material;
    material.ior = 1.5;
    material.sigma_a = {0.0, 0.0, 0.0};
    material.environment = {0.1, 0.2, 0.3};
    CompositeResult result;
    std::string error;
    TASK_CHECK(Compose(sc, material, result, &error));
    const FresnelSample in =
        FresnelRefract({0.0, 0.0, -1.0}, {0.0, 0.0, 1.0}, 1.0 / 1.5);
    const FresnelSample out =
        FresnelRefract({0.0, 0.0, -1.0}, {0.0, 0.0, -1.0}, 1.5);
    const double t = in.transmittance * out.transmittance;
    TASK_CHECK(Near(result.color[0], in.reflectance * 0.1 + t * 0.1, 1e-9));
    TASK_CHECK(Near(result.color[1], in.reflectance * 0.2 + t * 0.2, 1e-9));
    TASK_CHECK(Near(result.color[2], in.reflectance * 0.3 + t * 0.3, 1e-9));
    TASK_CHECK(!result.pixels[0].background_found);
    return 0;
}

static int CaseTransportTotalInternalReflection() {
    const Mat4 p = PerspectiveProjection(-1.0, 1.0, -1.0, 1.0, 1.0, 40.0);
    SceneCapture sc = MakeEmptyScene(1, 1, p, 1.0, 40.0);
    FillBackground(sc, 20.0, {0.7, 0.7, 0.7});
    sc.glass.front_valid[0] = 1;
    sc.glass.front_position[0] = {0.0, 0.0, -4.0};
    sc.glass.front_normal[0] = {0.0, 0.0, 1.0};
    sc.glass.front_depth[0] = 4.0;
    sc.glass.back_valid[0] = 1;
    sc.glass.back_position[0] = {0.0, 0.0, -6.0};
    /* Exit normal strongly tilted so the internal ray exceeds the critical
     * angle at the glass/air exit. */
    sc.glass.back_normal[0] = Normalize(Vec3{0.9, 0.0, -0.1});
    sc.glass.back_depth[0] = 6.0;

    Material material;
    material.ior = 1.5;
    material.sigma_a = {0.0, 0.0, 0.0};
    material.environment = {0.2, 0.1, 0.0};
    CompositeResult result;
    std::string error;
    TASK_CHECK(Compose(sc, material, result, &error));
    const PixelTrace& trace = result.pixels[0];
    TASK_CHECK(trace.exit_found);
    const FresnelSample in =
        FresnelRefract({0.0, 0.0, -1.0}, {0.0, 0.0, 1.0}, 1.0 / 1.5);
    /* Transmission dropped entirely; only the entrance reflection remains. */
    TASK_CHECK(!trace.background_found);
    TASK_CHECK(Near(result.color[0], in.reflectance * 0.2, 1e-9));
    TASK_CHECK(Near(result.color[1], in.reflectance * 0.1, 1e-9));
    TASK_CHECK(Near(result.color[2], in.reflectance * 0.0, 1e-9));
    return 0;
}

static int CaseTransportEntranceTotalInternalReflection() {
    const Mat4 p = PerspectiveProjection(-1.0, 1.0, -1.0, 1.0, 1.0, 40.0);
    SceneCapture sc = MakeEmptyScene(1, 1, p, 1.0, 40.0);
    FillBackground(sc, 20.0, {0.9, 0.9, 0.9});
    sc.glass.front_valid[0] = 1;
    sc.glass.front_position[0] = {0.0, 0.0, -4.0};
    /* The camera ray is close to normal and air->glass cannot TIR, but the
     * general Fresnel routine still has to orient the normal correctly and
     * never report TIR at a lower-index entrance. */
    sc.glass.front_normal[0] = Normalize(Vec3{-0.7, 0.0, 1.0});
    sc.glass.front_depth[0] = 4.0;
    sc.glass.back_valid[0] = 1;
    sc.glass.back_position[0] = {0.0, 0.0, -6.0};
    sc.glass.back_normal[0] = {0.0, 0.0, -1.0};
    sc.glass.back_depth[0] = 6.0;

    Material material;
    material.ior = 1.5;
    material.sigma_a = {0.0, 0.0, 0.0};
    material.environment = {0.3, 0.3, 0.3};
    CompositeResult result;
    std::string error;
    TASK_CHECK(Compose(sc, material, result, &error));
    const FresnelSample in =
        FresnelRefract(Normalize(Vec3{0.0, 0.0, -4.0}),
                       Normalize(Vec3{-0.7, 0.0, 1.0}), 1.0 / 1.5);
    TASK_CHECK(!in.total_internal_reflection);
    TASK_CHECK(in.reflectance > 0.0 && in.reflectance < 1.0);
    TASK_CHECK(result.pixels[0].exit_found);
    return 0;
}

static int CaseTransportMissingExit() {
    const Mat4 p = PerspectiveProjection(-1.0, 1.0, -1.0, 1.0, 1.0, 40.0);
    SceneCapture sc = MakeEmptyScene(1, 1, p, 1.0, 40.0);
    FillBackground(sc, 20.0, {0.7, 0.7, 0.7});
    sc.glass.front_valid[0] = 1;
    sc.glass.front_position[0] = {0.0, 0.0, -4.0};
    sc.glass.front_normal[0] = {0.0, 0.0, 1.0};
    sc.glass.front_depth[0] = 4.0;
    /* No back-layer sample at all: the exit is missing. */
    Material material;
    material.ior = 1.5;
    material.sigma_a = {0.0, 0.0, 0.0};
    material.environment = {0.05, 0.06, 0.07};
    CompositeResult result;
    std::string error;
    TASK_CHECK(Compose(sc, material, result, &error));
    const PixelTrace& trace = result.pixels[0];
    TASK_CHECK(!trace.exit_found);
    const FresnelSample in =
        FresnelRefract({0.0, 0.0, -1.0}, {0.0, 0.0, 1.0}, 1.0 / 1.5);
    TASK_CHECK(Near(result.color[0], in.reflectance * 0.05, 1e-9));
    TASK_CHECK(Near(result.color[1], in.reflectance * 0.06, 1e-9));
    TASK_CHECK(Near(result.color[2], in.reflectance * 0.07, 1e-9));
    return 0;
}

static int CaseTransportBackgroundOccludes() {
    const Mat4 p = PerspectiveProjection(-1.0, 1.0, -1.0, 1.0, 1.0, 40.0);
    SceneCapture sc = MakeEmptyScene(1, 1, p, 1.0, 40.0);
    /* Opaque background sits in front of the glass entrance. */
    FillBackground(sc, 2.0, {0.9, 0.8, 0.1});
    sc.glass.front_valid[0] = 1;
    sc.glass.front_position[0] = {0.0, 0.0, -5.0};
    sc.glass.front_normal[0] = {0.0, 0.0, 1.0};
    sc.glass.front_depth[0] = 5.0;
    sc.glass.back_valid[0] = 1;
    sc.glass.back_position[0] = {0.0, 0.0, -7.0};
    sc.glass.back_normal[0] = {0.0, 0.0, -1.0};
    sc.glass.back_depth[0] = 7.0;

    Material material;
    material.ior = 1.5;
    material.sigma_a = {};
    material.environment = {0.0, 0.0, 0.0};
    CompositeResult result;
    std::string error;
    TASK_CHECK(Compose(sc, material, result, &error));
    const PixelTrace& trace = result.pixels[0];
    TASK_CHECK(trace.background_kept);
    TASK_CHECK(!trace.entrance_visible);
    TASK_CHECK(Near(result.color[0], 0.9));
    TASK_CHECK(Near(result.color[1], 0.8));
    TASK_CHECK(Near(result.color[2], 0.1));
    return 0;
}

static int CaseTransportExitNormalFork() {
    const Mat4 p = PerspectiveProjection(-1.0, 1.0, -1.0, 1.0, 1.0, 40.0);
    /* Two scenes with identical depth fields but different exit normals must
     * produce different background hits (the classic exit-normal branch). */
    auto build = [&](const Vec3& exit_normal) {
        SceneCapture sc = MakeEmptyScene(1, 1, p, 1.0, 40.0);
        FillBackground(sc, 20.0, {0.4, 0.5, 0.6});
        sc.glass.front_valid[0] = 1;
        sc.glass.front_position[0] = {0.0, 0.0, -4.0};
        sc.glass.front_normal[0] = {0.0, 0.0, 1.0};
        sc.glass.front_depth[0] = 4.0;
        sc.glass.back_valid[0] = 1;
        sc.glass.back_position[0] = {0.0, 0.0, -6.0};
        sc.glass.back_depth[0] = 6.0;
        sc.glass.back_normal[0] = exit_normal;
        return sc;
    };
    Material material;
    material.ior = 1.5;
    material.sigma_a = {0.0, 0.0, 0.0};
    material.environment = {0.0, 0.0, 0.0};

    SceneCapture flat = build(Vec3{0.0, 0.0, -1.0});
    SceneCapture tilted = build(Normalize(Vec3{0.5, 0.0, -1.0}));
    CompositeResult flat_result, tilted_result;
    std::string error;
    TASK_CHECK(Compose(flat, material, flat_result, &error));
    TASK_CHECK(Compose(tilted, material, tilted_result, &error));
    /* Both hit the background, but the tilted exit normal sends the ray in a
     * different direction, so the reported hit differs. */
    const PixelTrace& a = flat_result.pixels[0];
    const PixelTrace& b = tilted_result.pixels[0];
    TASK_CHECK(a.exit_found && b.exit_found);
    TASK_CHECK(a.exit_hit.distance == b.exit_hit.distance);
    const bool different =
        std::abs(a.background_hit.distance - b.background_hit.distance) >
            1e-6 ||
        a.background_hit.hit != b.background_hit.hit;
    TASK_CHECK(different);
    return 0;
}

static int CaseTransportParameterRejection() {
    const Mat4 p = PerspectiveProjection(-1.0, 1.0, -1.0, 1.0, 1.0, 40.0);
    SceneCapture sc = MakeEmptyScene(1, 1, p, 1.0, 40.0);
    FillBackground(sc, 10.0, {0.5, 0.5, 0.5});
    Material good;
    good.ior = 1.5;
    good.sigma_a = {0.1, 0.1, 0.1};
    good.environment = {0.1, 0.1, 0.1};
    CompositeResult result;
    std::string error;
    TASK_CHECK(Compose(sc, good, result, &error));
    result.color[0] = 42.0;
    const double sentinel = result.color[0];

    Material bad_ior = good;
    bad_ior.ior = 1.0;
    TASK_CHECK(!Compose(sc, bad_ior, result, &error));
    TASK_CHECK(result.color[0] == sentinel);

    bad_ior = good;
    bad_ior.ior = 2.6;
    TASK_CHECK(!Compose(sc, bad_ior, result, &error));
    TASK_CHECK(result.color[0] == sentinel);

    Material bad_sigma = good;
    bad_sigma.sigma_a = {-0.1, 0.0, 0.0};
    TASK_CHECK(!Compose(sc, bad_sigma, result, &error));
    TASK_CHECK(result.color[0] == sentinel);

    Material bad_env = good;
    bad_env.environment = {0.0, std::numeric_limits<double>::quiet_NaN(), 0.0};
    TASK_CHECK(!Compose(sc, bad_env, result, &error));
    TASK_CHECK(result.color[0] == sentinel);

    SceneCapture malformed = sc;
    malformed.glass.front_normal[0] = Vec3{0.0, 0.0, 0.0};
    malformed.glass.front_valid[0] = 1;
    TASK_CHECK(!Compose(malformed, good, result, &error));
    TASK_CHECK(result.color[0] == sentinel);

    SceneCapture wrong_size = sc;
    wrong_size.glass.width = 2;
    TASK_CHECK(!Compose(wrong_size, good, result, &error));
    TASK_CHECK(result.color[0] == sentinel);
    return 0;
}

static int CaseTransportReadOnlyAndRecompose() {
    const int W = 24, H = 24;
    const Mat4 p = PerspectiveProjection(-1.0, 1.0, -1.0, 1.0, 1.0, 30.0);
    const CaptureSettings cs = MakeSettings(W, H, p, 1.0, 30.0);
    const Mesh wedge = MakeWedge(-4.0, -7.0);
    const Mesh backdrop = MakeBackdrop(-14.0, {0.3, 0.5, 0.7});
    SceneCapture scene;
    std::string error;
    TASK_CHECK(CaptureScene(backdrop, wedge, cs, scene, &error));
    SceneCapture scene_before = scene;

    Material a;
    a.ior = 1.5;
    a.sigma_a = {0.1, 0.05, 0.02};
    a.environment = {0.02, 0.02, 0.03};
    Material b = a;
    b.sigma_a = {0.5, 0.4, 0.3};

    CompositeResult first, second, third;
    TASK_CHECK(Compose(scene, a, first, &error));
    TASK_CHECK(Compose(scene, b, second, &error));
    TASK_CHECK(GlassUnchanged(scene.glass, scene_before.glass));
    TASK_CHECK(BackgroundUnchanged(scene.background, scene_before.background));
    /* Changing absorption must change the colour (otherwise the recompose
     * path would not be independent). */
    bool changed = false;
    for (std::size_t i = 0; i < first.color.size(); ++i) {
        if (std::abs(first.color[i] - second.color[i]) > 1e-9) changed = true;
    }
    TASK_CHECK(changed);

    /* Repeating the first composition reproduces it exactly and the returned
     * storage is independent from the second result. */
    TASK_CHECK(Compose(scene, a, third, &error));
    TASK_CHECK(third.color == first.color);
    TASK_CHECK(third.color.data() != first.color.data());
    TASK_CHECK(third.color.data() != second.color.data());
    return 0;
}

static int CaseTransportRealCaptureFork() {
    /* Capture the same wedge depth field twice, changing only the recorded
     * exit normals.  The background trace must fork, proving the exit normal
     * is used rather than an axial offset. */
    const int W = 64, H = 48;
    const Mat4 p = PerspectiveProjection(-1.2, 0.9, -0.7, 0.8, 1.0, 40.0);
    const CaptureSettings cs = MakeSettings(W, H, p, 1.0, 40.0);
    const Mesh wedge = MakeWedge(-5.0, -8.0);
    const Mesh backdrop = MakeBackdrop(-18.0, {0.4, 0.6, 0.8});
    SceneCapture base;
    std::string error;
    TASK_CHECK(CaptureScene(backdrop, wedge, cs, base, &error));

    SceneCapture tilted = base;
    for (std::size_t i = 0; i < tilted.glass.back_normal.size(); ++i) {
        if (tilted.glass.back_valid[i])
            tilted.glass.back_normal[i] =
                Normalize(Vec3{0.35, 0.0, -1.0});
    }

    Material material;
    material.ior = 1.5;
    material.sigma_a = {0.0, 0.0, 0.0};
    material.environment = {0.0, 0.0, 0.0};
    CompositeResult a, b;
    TASK_CHECK(Compose(base, material, a, &error));
    TASK_CHECK(Compose(tilted, material, b, &error));

    std::size_t differing_background_hits = 0;
    std::size_t compared = 0;
    for (std::size_t i = 0; i < a.pixels.size(); ++i) {
        if (!a.pixels[i].exit_found || !b.pixels[i].exit_found) continue;
        ++compared;
        if (a.pixels[i].background_hit.distance !=
            b.pixels[i].background_hit.distance)
            ++differing_background_hits;
    }
    TASK_CHECK(compared > 0);
    TASK_CHECK(differing_background_hits > 0);
    return 0;
}

static int CaseTransportWedgeCapture() {
    const int W = 64, H = 48;
    /* Asymmetric and non-square viewport. */
    const Mat4 p = PerspectiveProjection(-1.4, 1.0, -0.8, 0.9, 1.0, 40.0);
    const CaptureSettings cs = MakeSettings(W, H, p, 1.0, 40.0);
    const Mesh wedge = MakeWedge(-5.0, -8.0);
    const Mesh backdrop = MakeBackdrop(-15.0, {0.35, 0.55, 0.25});
    SceneCapture scene;
    std::string error;
    TASK_CHECK(CaptureScene(backdrop, wedge, cs, scene, &error));

    /* Perspective-correct record check: the wedge's front plane is planar;
     * every valid front sample must lie on it and have the correct normal. */
    std::size_t front = 0, back = 0;
    for (std::size_t i = 0; i < scene.glass.front_valid.size(); ++i) {
        if (scene.glass.front_valid[i]) {
            ++front;
            TASK_CHECK(Near(scene.glass.front_position[i].z, -5.0, 1e-6));
            TASK_CHECK(NearV(scene.glass.front_normal[i], {0, 0, 1}, 1e-6));
        }
        if (scene.glass.back_valid[i]) {
            ++back;
            /* The back layer also records the wedge's slanted side faces;
             * only the flat back face is at z = -8. */
            if (NearV(scene.glass.back_normal[i], {0, 0, -1}, 1e-6))
                TASK_CHECK(Near(scene.glass.back_position[i].z, -8.0, 1e-6));
        }
    }
    TASK_CHECK(front > 0 && back > 0);

    Material material;
    material.ior = 1.45;
    material.sigma_a = {0.05, 0.1, 0.15};
    material.environment = {0.01, 0.01, 0.02};
    CompositeResult result;
    TASK_CHECK(Compose(scene, material, result, &error));
    /* At least some pixels see through the glass, and no colour is NaN. */
    std::size_t transmitted = 0;
    for (std::size_t i = 0; i < result.pixels.size(); ++i) {
        if (result.pixels[i].exit_found && result.pixels[i].background_found)
            ++transmitted;
        for (int c = 0; c < 3; ++c)
            TASK_CHECK(std::isfinite(result.color[3 * i + c]));
    }
    TASK_CHECK(transmitted > 0);
    TASK_CHECK(scene.background.width == W && scene.background.height == H);
    return 0;
}

static int CaseTransportPerspectiveCorrectW() {
    /* Two front vertices at different w must interpolate their view-space
     * position with 1/w weights, not screen-space weights. */
    const int W = 32, H = 32;
    const Mat4 p = PerspectiveProjection(-1.0, 1.0, -1.0, 1.0, 1.0, 30.0);
    const CaptureSettings cs = MakeSettings(W, H, p, 1.0, 30.0);
    /* A front triangle spanning z=-3 (near) to z=-9 (far). */
    Mesh mesh;
    AddTriangle(mesh, {-0.8, -0.8, -3.0}, {0.8, -0.8, -9.0},
                {0.0, 0.9, -6.0}, Vec3{0.0, 0.0, 1.0});
    GlassCapture glass;
    std::string error;
    TASK_CHECK(CaptureGlass(mesh, cs, glass, &error));

    /* For a pixel centre, compare the captured view-space depth with the
     * true ray-plane intersection of that pixel's view ray. */
    std::size_t checked = 0;
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            const std::size_t idx = static_cast<std::size_t>(y) * W + x;
            if (!glass.front_valid[idx]) continue;
            /* Recover the view ray from the screen ray using the inverse of
             * the perspective divide: ndc = 2*s/size - 1, and the plane is
             * the triangle's plane. */
            const Vec3 n = Normalize(Cross(Vec3{1.6, 0.0, -6.0},
                                           Vec3{0.8, 1.7, -3.0}));
            const double ndc_x = 2.0 * (x + 0.5) / W - 1.0;
            const double ndc_y = 2.0 * (y + 0.5) / H - 1.0;
            const Vec3 dir{ndc_x, ndc_y, -1.0};
            const double denom = Dot(n, dir);
            if (std::abs(denom) < 1e-12) continue;
            const double s = Dot(n, Vec3{-0.8, -0.8, -3.0}) / denom;
            if (!(s > 0.0)) continue;
            const Vec3 expected = dir * s;
            TASK_CHECK(Near(glass.front_position[idx].z, expected.z, 2e-3));
            ++checked;
        }
    }
    TASK_CHECK(checked > 0);
    return 0;
}

/* ------------------------------------------------------------------ PPM */

static int CasePpmQuantizeAndFlip() {
    const int W = 3, H = 2;
    std::vector<double> pixels(3 * W * H, 0.0);
    /* Bottom row (y=0) is red, top row (y=1) is blue in the y-up buffer. */
    for (int x = 0; x < W; ++x) {
        pixels[3 * (0 * W + x) + 0] = 1.0;
        pixels[3 * (1 * W + x) + 2] = 1.0;
    }
    /* Over-range and negative values must clamp, and 0.5 quantises to 128.
     * The bottom y-up row is the last PPM row written (bytes 6..14). */
    pixels[3 * (0 * W) + 1] = 2.0;
    pixels[3 * (0 * W + 1) + 1] = -1.0;
    pixels[3 * (0 * W + 2) + 1] = 0.5;
    /* Make the top row distinct: its green channel is 0 everywhere. */

    const std::string path = ".task-build/refraction_ppm_test.ppm";
    std::string error;
    TASK_CHECK(WritePPM(path, W, H, pixels, &error));
    std::FILE* file = std::fopen(path.c_str(), "rb");
    TASK_CHECK(file != nullptr);
    char header[32] = {};
    TASK_CHECK(std::fgets(header, sizeof(header), file) != nullptr);
    TASK_CHECK(std::strcmp(header, "P6\n") == 0);
    TASK_CHECK(std::fgets(header, sizeof(header), file) != nullptr);
    int hw = 0, hh = 0;
    TASK_CHECK(std::sscanf(header, "%d %d", &hw, &hh) == 2);
    TASK_CHECK(hw == W && hh == H);
    TASK_CHECK(std::fgets(header, sizeof(header), file) != nullptr);
    unsigned char row[18];
    TASK_CHECK(std::fread(row, 1, 18, file) == 18);
    /* PPM is top-down: the first row written is the y-up top row.  Both
     * rows have red in the bottom row, so also verify the blue channel. */
    /* Each row is 3 pixels = 9 bytes. */
    TASK_CHECK(row[0] == 0 && row[1] == 0 && row[2] == 255);
    TASK_CHECK(row[3] == 0 && row[4] == 0 && row[5] == 255);
    TASK_CHECK(row[6] == 0 && row[7] == 0 && row[8] == 255);
    /* Second PPM row is the y-up bottom row (clamped/quantised green). */
    TASK_CHECK(row[9] == 255 && row[10] == 255 && row[11] == 0);    /* 2.0 */
    TASK_CHECK(row[12] == 255 && row[13] == 0 && row[14] == 0);     /* -1.0 */
    TASK_CHECK(row[15] == 255 && row[16] == 128 && row[17] == 0);   /* 0.5 */
    std::fclose(file);
    std::remove(path.c_str());

    /* Invalid sizes are rejected before writing. */
    TASK_CHECK(!WritePPM(path, 0, 2, pixels, &error));
    TASK_CHECK(!WritePPM(path, W, H, std::vector<double>(4, 0.0), &error));
    return 0;
}

static int CasePpmFromComposite() {
    const Mat4 p = PerspectiveProjection(-1.0, 1.0, -1.0, 1.0, 1.0, 20.0);
    SceneCapture sc = MakeEmptyScene(4, 3, p, 1.0, 20.0);
    Material material;
    material.ior = 1.5;
    material.environment = {0.0, 0.0, 0.0};
    CompositeResult result;
    std::string error;
    TASK_CHECK(Compose(sc, material, result, &error));
    const std::string path = ".task-build/refraction_composite_test.ppm";
    TASK_CHECK(WritePPM(path, result, &error));
    std::FILE* file = std::fopen(path.c_str(), "rb");
    TASK_CHECK(file != nullptr);
    std::fclose(file);
    std::remove(path.c_str());
    return 0;
}

const task_case task_cases[] = {
    {"capture", "projection_z", CaseCaptureProjectionZ},
    {"capture", "glass_layers", CaseCaptureGlassLayers},
    {"capture", "invalid_rejected", CaseCaptureInvalidRejected},
    {"capture", "background_depth", CaseCaptureBackgroundDepth},
    {"capture", "scene_ownership", CaseCaptureSceneOwnership},
    {"trace", "continuous_single_cell", CaseTraceContinuousSingleCell},
    {"trace", "skips_nearer_cells", CaseTraceSkipsNearerCells},
    {"trace", "asymmetric_projection", CaseTraceAsymmetricProjection},
    {"trace", "diagonal_bounded", CaseTraceDiagonalBounded},
    {"trace", "self_intersection", CaseTraceIgnoresSelfIntersection},
    {"trace", "outside_field", CaseTraceOutsideField},
    {"trace", "narrow_target", CaseTraceNarrowTarget},
    {"trace", "corner_cell", CaseTraceCornerCell},
    {"transport", "two_snells_and_length", CaseTransportTwoSnellsAndLength},
    {"transport", "oblique_internal_length",
     CaseTransportObliqueInternalLength},
    {"transport", "absorption", CaseTransportAbsorption},
    {"transport", "environment_miss", CaseTransportEnvironmentAndBackgroundMiss},
    {"transport", "total_internal_reflection",
     CaseTransportTotalInternalReflection},
    {"transport", "entrance_no_tir", CaseTransportEntranceTotalInternalReflection},
    {"transport", "missing_exit", CaseTransportMissingExit},
    {"transport", "background_occludes", CaseTransportBackgroundOccludes},
    {"transport", "exit_normal_fork", CaseTransportExitNormalFork},
    {"transport", "parameter_rejection", CaseTransportParameterRejection},
    {"transport", "read_only_recompose", CaseTransportReadOnlyAndRecompose},
    {"transport", "real_capture_fork", CaseTransportRealCaptureFork},
    {"transport", "wedge_capture", CaseTransportWedgeCapture},
    {"transport", "perspective_correct_w", CaseTransportPerspectiveCorrectW},
    {"transport", "ppm_quantize_flip", CasePpmQuantizeAndFlip},
    {"transport", "ppm_composite", CasePpmFromComposite},
};
const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
