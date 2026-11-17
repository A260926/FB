#include <cmath>
#include <cstdint>
#include <vector>

#include "task_support/check.h"
#include "src/refraction.h"

using namespace gfx;
using namespace gfx::refr;

namespace {

constexpr float kEps = 1e-4f;

auto Approx(float a, float b, float tol = kEps) -> bool {
    return std::isfinite(a) && std::isfinite(b) && std::abs(a - b) <= tol;
}

auto ApproxV(const Vec3& a, const Vec3& b, float tol = kEps) -> bool {
    return Approx(a.x, b.x, tol) && Approx(a.y, b.y, tol) && Approx(a.z, b.z, tol);
}

auto V4(float x, float y, float z, float w = 1.f) -> Vec { return Vec({x, y, z, w}); }
auto N4(float x, float y, float z) -> Vec { return Vec({x, y, z, 0.f}); }

auto MakeTriangle(std::initializer_list<Vec> pos, std::initializer_list<Vec> nrm)
    -> Triangle {
    Triangle t;
    const Vec* p = pos.begin();
    const Vec* n = nrm.begin();
    for (int i = 0; i < 3; ++i) {
        t.position[i] = p[i];
        t.normal[i] = n[i];
    }
    return t;
}

// Axis-aligned closed box centred at centre with half extents he; outward
// normals, CCW outward winding, 12 triangles.
auto MakeBox(const Vec3& centre, const Vec3& he) -> std::vector<Triangle> {
    Vec3 c[8] = {
        {centre.x - he.x, centre.y - he.y, centre.z - he.z},
        {centre.x + he.x, centre.y - he.y, centre.z - he.z},
        {centre.x + he.x, centre.y + he.y, centre.z - he.z},
        {centre.x - he.x, centre.y + he.y, centre.z - he.z},
        {centre.x - he.x, centre.y - he.y, centre.z + he.z},
        {centre.x + he.x, centre.y - he.y, centre.z + he.z},
        {centre.x + he.x, centre.y + he.y, centre.z + he.z},
        {centre.x - he.x, centre.y + he.y, centre.z + he.z}
    };
    // face = outward normal + four CCW corner indices when viewed from
    // outside (cross(edge1, edge2) equals the listed normal).
    struct FaceDef { int n[3]; int idx[4]; };
    const FaceDef faces[6] = {
        {{0, 0, -1}, {3, 2, 1, 0}}, // -z
        {{0, 0, 1}, {5, 6, 7, 4}},  // +z
        {{-1, 0, 0}, {4, 7, 3, 0}}, // -x
        {{1, 0, 0}, {2, 6, 5, 1}},  // +x
        {{0, -1, 0}, {1, 5, 4, 0}}, // -y
        {{0, 1, 0}, {7, 6, 2, 3}}   // +y
    };
    std::vector<Triangle> out;
    for (const auto& f : faces) {
        Vec3 n(static_cast<float>(f.n[0]), static_cast<float>(f.n[1]),
               static_cast<float>(f.n[2]));
        Vec nv[4];
        Vec3 pv[4];
        for (int k = 0; k < 4; ++k) {
            pv[k] = c[f.idx[k]];
            nv[k] = ToVec4(n, 0.f);
        }
        out.push_back({{ToVec4(pv[0], 1.f), ToVec4(pv[1], 1.f), ToVec4(pv[2], 1.f)},
                       {nv[0], nv[1], nv[2]}});
        out.push_back({{ToVec4(pv[0], 1.f), ToVec4(pv[2], 1.f), ToVec4(pv[3], 1.f)},
                       {nv[0], nv[2], nv[3]}});
    }
    return out;
}

// Closed wedge: a box sheared in x so the front and back planar faces are
// slanted (non-axis-aligned), keeping a watertight convex hull.
auto MakeWedge(const Vec3& centre, const Vec3& he, float shear) -> std::vector<Triangle> {
    Vec3 c[8] = {
        {centre.x - he.x + shear * (-he.z - centre.z), centre.y - he.y, centre.z - he.z},
        {centre.x + he.x + shear * (-he.z - centre.z), centre.y - he.y, centre.z - he.z},
        {centre.x + he.x + shear * (-he.z - centre.z), centre.y + he.y, centre.z - he.z},
        {centre.x - he.x + shear * (-he.z - centre.z), centre.y + he.y, centre.z - he.z},
        {centre.x - he.x + shear * (he.z - centre.z), centre.y - he.y, centre.z + he.z},
        {centre.x + he.x + shear * (he.z - centre.z), centre.y - he.y, centre.z + he.z},
        {centre.x + he.x + shear * (he.z - centre.z), centre.y + he.y, centre.z + he.z},
        {centre.x - he.x + shear * (he.z - centre.z), centre.y + he.y, centre.z + he.z}
    };
    struct FaceDef { int n[3]; int idx[4]; };
    // Recompute true outward normals from the corner geometry.
    auto normal_of = [&](int a, int b, int cc) {
        return Normalize(Cross(c[b] - c[a], c[cc] - c[a]));
    };
    const int loops[6][4] = {
        {3, 2, 1, 0}, {5, 6, 7, 4}, {4, 7, 3, 0},
        {2, 6, 5, 1}, {1, 5, 4, 0}, {7, 6, 2, 3}
    };
    std::vector<Triangle> out;
    for (const auto& loop : loops) {
        Vec3 n = normal_of(loop[0], loop[1], loop[2]);
        Vec nv[4];
        Vec3 pv[4];
        for (int k = 0; k < 4; ++k) {
            pv[k] = c[loop[k]];
            nv[k] = ToVec4(n, 0.f);
        }
        out.push_back({{ToVec4(pv[0], 1.f), ToVec4(pv[1], 1.f), ToVec4(pv[2], 1.f)},
                       {nv[0], nv[1], nv[2]}});
        out.push_back({{ToVec4(pv[0], 1.f), ToVec4(pv[2], 1.f), ToVec4(pv[3], 1.f)},
                       {nv[0], nv[2], nv[3]}});
    }
    return out;
}

void RasterizeAll(DepthTarget& target, const Mat4& view, const Mat4& proj,
                  const Mat4& normal, const std::vector<Triangle>& tris, FaceMode mode) {
    for (const auto& t : tris)
        target.RasterizeTriangle(view, proj, normal, t, mode);
}

void RasterizeAll(BackgroundTarget& target, const Mat4& view, const Mat4& proj,
                  const std::vector<Triangle>& tris, const Vec3& color) {
    for (const auto& t : tris)
        target.RasterizeTriangle(view, proj, t, color);
}

// ---------------------------------------------------------------------------
// capture group
// ---------------------------------------------------------------------------

int capture_pixel_conventions() {
    // Non-square target, symmetric frustum, identity world->view.
    const int W = 5, H = 4;
    Mat4 view = IdentityMat4();
    Mat4 normal = IdentityMat4();
    Mat4 proj = MakePerspective(-1.f, 1.f, -1.f, 1.f, 1.f, 50.f);
    DepthTarget target(W, H);

    // Single front-facing triangle in the z = -10 plane spanning most of view.
    Triangle t = MakeTriangle(
        {V4(-8.f, -8.f, -10.f), V4(8.f, -8.f, -10.f), V4(0.f, 8.f, -10.f)},
        {N4(0, 0, 1), N4(0, 0, 1), N4(0, 0, 1)});
    target.RasterizeTriangle(view, proj, normal, t, FaceMode::FrontFaces);

    // Centre pixel must be covered; eye depth stored, not NDC z.
    const CellRecord& centre = target.cell(W / 2, H / 2);
    TASK_CHECK(centre.valid);
    TASK_CHECK(Approx(centre.view_depth, 10.f, 1e-3f));
    TASK_CHECK(Approx(centre.position.z, -10.f, 1e-3f));
    TASK_CHECK(ApproxV(centre.normal, {0.f, 0.f, 1.f}));

    // A corner cell outside the triangle stays invalid.
    TASK_CHECK(!target.cell(0, H - 1).valid);

    // Re-running with a nearer triangle overwrites only covered nearer cells.
    Triangle near_tri = MakeTriangle(
        {V4(-8.f, -8.f, -4.f), V4(8.f, -8.f, -4.f), V4(0.f, 8.f, -4.f)},
        {N4(0, 0, 1), N4(0, 0, 1), N4(0, 0, 1)});
    target.RasterizeTriangle(view, proj, normal, near_tri, FaceMode::FrontFaces);
    TASK_CHECK(Approx(target.cell(W / 2, H / 2).view_depth, 4.f, 1e-3f));
    return 0;
}

int capture_backface_bypass_and_interpolation() {
    const int W = 12, H = 10;
    Mat4 view = IdentityMat4();
    Mat4 normal = IdentityMat4();
    Mat4 proj = MakePerspective(-1.f, 1.f, -1.f, 1.f, 1.f, 50.f);

    // A back-facing (clockwise) triangle with a slanted, non-unit-per-vertex
    // normal field: interpolation must be perspective correct and only the
    // final normal gets normalized.
    DepthTarget front(W, H), back(W, H);
    Triangle back_facing = MakeTriangle(
        {V4(6.f, -6.f, -12.f), V4(-6.f, -6.f, -12.f), V4(0.f, 6.f, -12.f)},
        {N4(0, 0, -1), N4(0, 0, -1), N4(0, 0, -1)});
    front.RasterizeTriangle(view, proj, normal, back_facing, FaceMode::FrontFaces);
    TASK_CHECK(!front.cell(W / 2, H / 2).valid); // original culling removes it
    back.RasterizeTriangle(view, proj, normal, back_facing, FaceMode::BackFaces);
    TASK_CHECK(back.cell(W / 2, H / 2).valid);   // back layer keeps it
    TASK_CHECK(Approx(back.cell(W / 2, H / 2).view_depth, 12.f, 1e-3f));
    TASK_CHECK(ApproxV(back.cell(W / 2, H / 2).normal, {0.f, 0.f, -1.f}));

    // Different clip w across vertices still yields correct perspective depth.
    // Reversed winding so this slanted surface is screen-clockwise (back face).
    const Vec p0 = V4(-6.f, -6.f, -6.f), p1 = V4(0.f, 6.f, -12.f), p2 = V4(6.f, -6.f, -18.f);
    Triangle slanted = MakeTriangle(
        {p0, p1, p2},
        {N4(0, 0, -1), N4(0, 0, -1), N4(0, 0, -1)});
    DepthTarget slant(W, H);
    slant.RasterizeTriangle(view, proj, normal, slanted, FaceMode::BackFaces);
    const CellRecord& rec = slant.cell(W / 2, H / 2);
    TASK_CHECK(rec.valid);

    // Independent expectation: signed-area screen barycentrics (a different
    // formula from the rasterizer's Gram-matrix one), then 1/w interpolation.
    auto screen_of = [&](const Vec& wp) {
        Vec cl = proj * wp;
        return std::array<float, 2>{(cl[0] / cl[3] + 1.f) * 0.5f * W,
                                    (cl[1] / cl[3] + 1.f) * 0.5f * H};
    };
    auto s0 = screen_of(p0), s1 = screen_of(p1), s2 = screen_of(p2);
    const float px = W / 2 + 0.5f, py = H / 2 + 0.5f;
    const float denom = (s1[1] - s2[1]) * (s0[0] - s2[0])
                      + (s2[0] - s1[0]) * (s0[1] - s2[1]);
    const float u = ((s1[1] - s2[1]) * (px - s2[0]) + (s2[0] - s1[0]) * (py - s2[1])) / denom;
    const float v = ((s2[1] - s0[1]) * (px - s2[0]) + (s0[0] - s2[0]) * (py - s2[1])) / denom;
    const float ww = 1.f - u - v;
    const float d0 = 6.f, d1 = 12.f, d2 = 18.f; // positive eye depths
    const float bary_depth = 1.f / (u / d0 + v / d1 + ww / d2);
    TASK_CHECK(Approx(rec.view_depth, bary_depth, 2e-3f));
    TASK_CHECK(Approx(-rec.position.z, rec.view_depth, 1e-3f));
    return 0;
}

int capture_asymmetric_and_non_square() {
    // Non-square target with an asymmetric off-centre frustum.
    const int W = 9, H = 3;
    Mat4 view = IdentityMat4();
    Mat4 normal = IdentityMat4();
    Mat4 proj = MakePerspective(-0.5f, 1.0f, -0.8f, 0.4f, 2.f, 40.f);
    DepthTarget target(W, H);
    Triangle t = MakeTriangle(
        {V4(-8.f, -8.f, -10.f), V4(8.f, -8.f, -10.f), V4(8.f, 8.f, -10.f)},
        {N4(0, 0, 1), N4(0, 0, 1), N4(0, 0, 1)});
    target.RasterizeTriangle(view, proj, normal, t, FaceMode::FrontFaces);

    // The NDC origin maps off-centre on the target: ndc (0,0) lands at
    // x = 0.5/1.5 * W, y = 0.8/1.2 * H.
    Vec clip = proj * V4(0.f, 0.f, -10.f);
    const float ndc_x = clip[0] / clip[3];
    const float ndc_y = clip[1] / clip[3];
    const int expect_x = static_cast<int>(std::floor((ndc_x + 1.f) * 0.5f * W));
    const int expect_y = static_cast<int>(std::floor((ndc_y + 1.f) * 0.5f * H));
    TASK_CHECK(target.cell(expect_x, expect_y).valid);
    TASK_CHECK(Approx(target.cell(expect_x, expect_y).view_depth, 10.f, 1e-3f));
    return 0;
}

int capture_wedge_layers_and_ownership() {
    // Programmatically generated closed convex wedge through the real capture
    // entry point; front and back shells must both populate, and a second
    // synthesis must not mutate the captures.
    const int W = 24, H = 18;
    Mat4 view = IdentityMat4();
    Mat4 normal = IdentityMat4();
    Mat4 proj = MakePerspective(-1.f, 1.f, -1.f, 1.f, 1.f, 60.f);
    auto mesh = MakeWedge({0.f, 0.f, -16.f}, {3.f, 3.f, 3.f}, 0.25f);

    DepthTarget front(W, H), back(W, H);
    RasterizeAll(front, view, proj, normal, mesh, FaceMode::FrontFaces);
    RasterizeAll(back, view, proj, normal, mesh, FaceMode::BackFaces);

    int front_count = 0, back_count = 0;
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            if (front.cell(x, y).valid) {
                ++front_count;
                TASK_CHECK(front.cell(x, y).view_depth < back.cell(x, y).view_depth
                           || !back.cell(x, y).valid);
            }
            if (back.cell(x, y).valid) {
                ++back_count;
                TASK_CHECK(Approx(Length(back.cell(x, y).normal), 1.f, 1e-3f));
            }
        }
    }
    TASK_CHECK(front_count > 10);
    TASK_CHECK(back_count > 10);

    BackgroundTarget bg(W, H);
    auto wall = MakeBox({0.f, 0.f, -34.f}, {30.f, 30.f, 1.f});
    RasterizeAll(bg, view, proj, wall, {0.2f, 0.4f, 0.8f});

    GlassMaterial mat;
    mat.ior = 1.5f;
    mat.sigma_a = {0.1f, 0.2f, 0.3f};
    SynthesisResult first = Synthesize(front, back, bg, proj, mat, {0.05f, 0.05f, 0.05f});
    const auto snapshot = [&]() {
        std::vector<float> d(front.size());
        for (size_t i = 0; i < front.size(); ++i) d[i] = front.cell(i % W, i / W).view_depth;
        return d;
    }();
    SynthesisResult second = Synthesize(front, back, bg, proj, mat, {0.05f, 0.05f, 0.05f});
    for (size_t i = 0; i < snapshot.size(); ++i) {
        const CellRecord& c = front.cell(i % W, i / W);
        TASK_CHECK(!c.valid || Approx(c.view_depth, snapshot[i], 0.f));
    }
    TASK_CHECK(first.color.size() == second.color.size());
    for (size_t i = 0; i < first.color.size(); ++i)
        TASK_CHECK(ApproxV(first.color[i], second.color[i], 0.f));
    return 0;
}

// ---------------------------------------------------------------------------
// trace group
// ---------------------------------------------------------------------------

// A slanted interface; the ray geometry is checked analytically against
// Snell's law using angles relative to the interface normal.
int trace_snell_directions() {
    const float ior = 1.5f;

    // Normal incidence first: the ray direction is unchanged.
    Vec3 face_n{0.f, 0.f, 1.f};
    Vec3 straight{0.f, 0.f, -1.f};
    Vec3 out_straight;
    float cos0 = 0.f;
    TASK_CHECK(RefractSnell(straight, face_n, 1.f / ior, &out_straight, &cos0));
    TASK_CHECK(ApproxV(out_straight, straight, 1e-6f));
    TASK_CHECK(Approx(cos0, 1.f));

    // Tilted entry interface: outward normal 30 degrees off the camera ray.
    const float ang = 30.f * 3.14159265f / 180.f;
    Vec3 normal{std::sin(ang), 0.f, std::cos(ang)}; // points back toward camera
    Vec3 incident{0.f, 0.f, -1.f};
    Vec3 refracted;
    float cosi = 0.f;
    TASK_CHECK(RefractSnell(incident, normal, 1.f / ior, &refracted, &cosi));
    TASK_CHECK(Approx(cosi, std::cos(ang)));
    TASK_CHECK(Approx(Length(refracted), 1.f, 1e-6f));
    // The transmitted ray makes angle theta_t against the (opposite) normal;
    // sin(theta_t) = sin(theta_i) / ior is Snell's law itself.
    const float sin_t = std::sin(ang) / ior;
    const float cos_t = std::cos(std::asin(sin_t));
    TASK_CHECK(Approx(-Dot(refracted, normal), cos_t, 1e-5f));
    // Tangential component projected onto the interface tangent (cos, 0, -sin).
    const Vec3 tangent{std::cos(ang), 0.f, -std::sin(ang)};
    TASK_CHECK(Approx(Dot(refracted, tangent), sin_t, 1e-5f));

    // Exit through a parallel interface (outward normal flipped): reciprocity
    // of Snell's law restores the original incident direction.
    Vec3 out_ray;
    float cos_exit = 0.f;
    TASK_CHECK(RefractSnell(refracted, normal * -1.f, ior, &out_ray, &cos_exit));
    TASK_CHECK(ApproxV(out_ray, incident, 2e-6f));
    return 0;
}

int trace_continuous_grid_intersection() {
    // Hand-built depth field: one cell carries a known plane depth; an exact
    // ray must hit it via the continuous parameter, never by stepping.
    const int W = 8, H = 6;
    Mat4 proj = MakePerspective(-1.f, 1.f, -1.f, 1.f, 1.f, 50.f);
    DepthTarget field(W, H);
    // Fill every cell with the z = -20 plane.
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            CellRecord& c = field.cell(x, y);
            c.valid = true;
            c.view_depth = 20.f;
            c.position = {0.f, 0.f, -20.f};
            c.normal = {0.f, 0.f, -1.f};
        }

    Vec3 origin{0.f, 0.f, -10.f};
    Vec3 direction = Normalize({0.3f, 0.2f, -1.f});
    ExitTrace tr = TraceDepthField(field, proj, origin, direction);
    TASK_CHECK(tr.hit);
    const float expect_t = 10.f / -direction.z;
    TASK_CHECK(Approx(static_cast<float>(tr.exit_t), expect_t, 1e-3f));
    TASK_CHECK(Approx(tr.exit_position.z, -20.f, 1e-3f));
    TASK_CHECK(tr.cells_visited >= 1);
    TASK_CHECK(tr.cells_visited <= static_cast<size_t>(W + H + 1));
    return 0;
}

int trace_narrow_target_and_border() {
    // Ray leaving the narrow image must stop at the border with ExitMissing.
    const int W = 3, H = 12;
    Mat4 proj = MakePerspective(-1.f, 1.f, -1.f, 1.f, 1.f, 50.f);
    DepthTarget field(W, H); // all invalid
    Vec3 origin{0.f, 0.f, -5.f};
    Vec3 direction = Normalize({1.f, 0.05f, -1.f});
    ExitTrace tr = TraceDepthField(field, proj, origin, direction);
    TASK_CHECK(!tr.hit);
    TASK_CHECK(tr.status == TraceStatus::ExitMissing);
    TASK_CHECK(tr.cells_visited <= static_cast<size_t>(W + H + 1));

    // A single valid cell in the far corner is reachable only via the exact
    // crossing; cells are never visited twice.
    DepthTarget one(W, H);
    CellRecord& c = one.cell(W - 1, H / 2);
    c.valid = true;
    c.view_depth = 30.f;
    c.position = {0.f, 0.f, -30.f};
    c.normal = {0.f, 0.f, -1.f};
    // Aim at the world point whose projection is that cell centre at z = -30.
    const float cell_ndc_x = ((W - 1) + 0.5f) / W * 2.f - 1.f;
    const float world_x = cell_ndc_x * 30.f; // P00 == 1 for this symmetric frustum
    Vec3 aimed = Normalize(Vec3{world_x, 0.f, -30.f} - origin);
    ExitTrace hit = TraceDepthField(one, proj, origin, aimed);
    TASK_CHECK(hit.hit);
    TASK_CHECK(hit.cell_x == W - 1);
    return 0;
}

int trace_corner_crossing_visit_budget() {
    // Diagonal ray crossing many cell corners: each cell once, bounded visits.
    const int W = 16, H = 16;
    Mat4 proj = MakePerspective(-1.f, 1.f, -1.f, 1.f, 1.f, 80.f);
    DepthTarget field(W, H);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            CellRecord& c = field.cell(x, y);
            c.valid = true;
            c.view_depth = 60.f;
            c.position = {0.f, 0.f, -60.f};
            c.normal = {0.f, 0.f, -1.f};
        }
    Vec3 origin{0.f, 0.f, -4.f};
    Vec3 direction = Normalize({1.f, 1.f, -1.f});
    ExitTrace tr = TraceDepthField(field, proj, origin, direction);
    TASK_CHECK(tr.hit);
    TASK_CHECK(tr.cells_visited <= static_cast<size_t>(W + H + 1));
    TASK_CHECK(Approx(tr.exit_position.z, -60.f, 1e-2f));
    return 0;
}

int trace_same_depth_different_normals_fork() {
    // Identical depth field, two possible exit normals: the outgoing rays and
    // therefore background hit pixels must diverge.
    const int W = 12, H = 12;
    Mat4 proj = MakePerspective(-1.f, 1.f, -1.f, 1.f, 1.f, 60.f);
    auto build = [&](Vec3 normal) {
        DepthTarget field(W, H);
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x) {
                CellRecord& c = field.cell(x, y);
                c.valid = true;
                c.view_depth = 24.f;
                c.position = {0.f, 0.f, -24.f};
                c.normal = normal;
            }
        return field;
    };
    DepthTarget flat = build({0.f, 0.f, -1.f});
    DepthTarget tilted = build(Normalize({0.4f, 0.f, -1.f}));

    BackgroundTarget bg(W, H);
    auto wall = MakeBox({0.f, 0.f, -40.f}, {30.f, 30.f, 1.f});
    Mat4 view = IdentityMat4();
    RasterizeAll(bg, view, proj, wall, {0.5f, 0.5f, 0.5f});

    Vec3 entry{0.f, 0.f, -12.f};
    Vec3 internal_dir = Normalize({0.2f, 0.f, -1.f});
    ExitTrace a = TraceDepthField(flat, proj, entry, internal_dir);
    ExitTrace b = TraceDepthField(tilted, proj, entry, internal_dir);
    TASK_CHECK(a.hit && b.hit);
    TASK_CHECK(Approx(static_cast<float>(a.exit_t), static_cast<float>(b.exit_t), 1e-3f));

    Vec3 out_a, out_b;
    float ca = 0.f, cb = 0.f;
    TASK_CHECK(RefractSnell(internal_dir, a.exit_normal, 1.5f, &out_a, &ca));
    TASK_CHECK(RefractSnell(internal_dir, b.exit_normal, 1.5f, &out_b, &cb));
    TASK_CHECK(!ApproxV(out_a, out_b, 1e-3f));
    BackgroundTrace ba = TraceBackground(bg, proj, a.exit_position, out_a);
    BackgroundTrace bb = TraceBackground(bg, proj, b.exit_position, out_b);
    TASK_CHECK(ba.hit && bb.hit);
    TASK_CHECK(ba.cell_x != bb.cell_x || ba.cell_y != bb.cell_y);
    return 0;
}

// ---------------------------------------------------------------------------
// transport group
// ---------------------------------------------------------------------------

int transport_fresnel_and_absorption() {
    // Flat parallel slab with known thickness: check the colour formula
    // including the true internal path length and Beer-Lambert attenuation.
    const int W = 9, H = 9;
    Mat4 view = IdentityMat4();
    Mat4 normal = IdentityMat4();
    Mat4 proj = MakePerspective(-1.f, 1.f, -1.f, 1.f, 1.f, 60.f);

    // One closed box: its near shell (z=-14) is the entry, far shell (z=-16)
    // the exit, giving an on-axis internal path of length 2.
    auto glass = MakeBox({0.f, 0.f, -15.f}, {4.f, 4.f, 1.f});
    DepthTarget front(W, H), back(W, H);
    RasterizeAll(front, view, proj, normal, glass, FaceMode::FrontFaces);
    RasterizeAll(back, view, proj, normal, glass, FaceMode::BackFaces);

    BackgroundTarget bg(W, H);
    auto wall = MakeBox({0.f, 0.f, -30.f}, {30.f, 30.f, 1.f});
    RasterizeAll(bg, view, proj, wall, {0.8f, 0.6f, 0.4f});

    GlassMaterial mat;
    mat.ior = 1.5f;
    mat.sigma_a = {0.25f, 0.5f, 0.75f};
    const Vec3 ambient{0.1f, 0.2f, 0.3f};
    SynthesisResult res = Synthesize(front, back, bg, proj, mat, ambient);

    const int x = W / 2, y = H / 2;
    TASK_CHECK(front.cell(x, y).valid);
    // Internal length: slab is 2 deep; central ray is on axis, so length == 2.
    const float length = 2.f;
    const float f = FresnelUnpolarized(1.f, 1.f / 1.5f);
    const Vec3 expected = ambient * f
        + Vec3{0.8f, 0.6f, 0.4f}
          * Vec3{std::exp(-0.25f * length), std::exp(-0.5f * length),
                 std::exp(-0.75f * length)}
          * ((1.f - f) * (1.f - f));
    TASK_CHECK(ApproxV(res.color[y * W + x], expected, 2e-3f));
    TASK_CHECK(res.stats.exit_hits >= 1);
    return 0;
}

int transport_total_internal_reflection() {
    // Steep internal ray against a near-grazing exit normal must TIR: the
    // transmission term vanishes and only the entry reflection survives.
    const int W = 10, H = 10;
    Mat4 proj = MakePerspective(-1.f, 1.f, -1.f, 1.f, 1.f, 60.f);
    DepthTarget front(W, H), back(W, H);
    for (int yy = 0; yy < H; ++yy)
        for (int xx = 0; xx < W; ++xx) {
            CellRecord& cf = front.cell(xx, yy);
            cf.valid = true; cf.view_depth = 10.f;
            cf.position = {0.f, 0.f, -10.f}; cf.normal = {0.f, 0.f, 1.f};
            CellRecord& cb = back.cell(xx, yy);
            cb.valid = true; cb.view_depth = 14.f;
            cb.position = {0.f, 0.f, -14.f};
            // Outward normal tilted beyond the glass->air critical angle.
            cb.normal = Normalize({0.9f, 0.f, -0.1f});
        }
    BackgroundTarget bg(W, H);
    auto wall = MakeBox({0.f, 0.f, -30.f}, {30.f, 30.f, 1.f});
    Mat4 view = IdentityMat4();
    RasterizeAll(bg, view, proj, wall, {0.9f, 0.9f, 0.9f});

    GlassMaterial mat;
    mat.ior = 1.5f;
    const Vec3 ambient{0.2f, 0.2f, 0.2f};
    SynthesisResult res = Synthesize(front, back, bg, proj, mat, ambient);
    TASK_CHECK(res.stats.total_internal_reflection >= 1);
    const Vec3 got = res.color[(H / 2) * W + W / 2];
    const float f = FresnelUnpolarized(1.f, 1.f / 1.5f);
    TASK_CHECK(ApproxV(got, ambient * f, 2e-3f));
    return 0;
}

int transport_exit_missing() {
    // Back layer with a hole where the internal ray travels: no exit, so the
    // transmission contribution is zero and reflection is retained.
    const int W = 11, H = 11;
    Mat4 proj = MakePerspective(-1.f, 1.f, -1.f, 1.f, 1.f, 60.f);
    DepthTarget front(W, H), back(W, H);
    for (int yy = 0; yy < H; ++yy)
        for (int xx = 0; xx < W; ++xx) {
            CellRecord& cf = front.cell(xx, yy);
            cf.valid = true; cf.view_depth = 10.f;
            cf.position = {0.f, 0.f, -10.f}; cf.normal = {0.f, 0.f, 1.f};
            // Leave the central column invalid so the axis ray exits through
            // nothing; neighbour columns still hold a back surface.
            if (xx != W / 2) {
                CellRecord& cb = back.cell(xx, yy);
                cb.valid = true; cb.view_depth = 14.f;
                cb.position = {0.f, 0.f, -14.f}; cb.normal = {0.f, 0.f, -1.f};
            }
        }
    BackgroundTarget bg(W, H);
    auto wall = MakeBox({0.f, 0.f, -30.f}, {30.f, 30.f, 1.f});
    Mat4 view = IdentityMat4();
    RasterizeAll(bg, view, proj, wall, {0.7f, 0.7f, 0.7f});

    GlassMaterial mat;
    mat.ior = 1.5f;
    const Vec3 ambient{0.15f, 0.15f, 0.15f};
    SynthesisResult res = Synthesize(front, back, bg, proj, mat, ambient);
    TASK_CHECK(res.stats.exit_missing >= 1);
    const float f = FresnelUnpolarized(1.f, 1.f / 1.5f);
    TASK_CHECK(ApproxV(res.color[(H / 2) * W + W / 2], ambient * f, 2e-3f));
    return 0;
}

int transport_background_occlusion_and_miss() {
    // Opaque background in front of the glass entry: entry keeps background.
    const int W = 9, H = 9;
    Mat4 view = IdentityMat4();
    Mat4 normal = IdentityMat4();
    Mat4 proj = MakePerspective(-1.f, 1.f, -1.f, 1.f, 1.f, 60.f);
    auto glass = MakeBox({0.f, 0.f, -16.f}, {3.f, 3.f, 1.f});
    DepthTarget front(W, H), back(W, H);
    RasterizeAll(front, view, proj, normal, glass, FaceMode::FrontFaces);
    RasterizeAll(back, view, proj, normal, glass, FaceMode::BackFaces);

    BackgroundTarget near_bg(W, H);
    auto near_wall = MakeBox({0.f, 0.f, -8.f}, {30.f, 30.f, 1.f});
    RasterizeAll(near_bg, view, proj, near_wall, {0.3f, 0.3f, 0.3f});
    GlassMaterial mat; mat.ior = 1.5f;
    SynthesisResult blocked = Synthesize(front, back, near_bg, proj, mat, {0.f, 0.f, 0.f});
    TASK_CHECK(blocked.stats.occluded_entries >= 1);
    TASK_CHECK(ApproxV(blocked.color[(H / 2) * W + W / 2], {0.3f, 0.3f, 0.3f}, 1e-6f));

    // Empty background: rays miss and fall back to ambient.
    BackgroundTarget empty(W, H);
    SynthesisResult miss = Synthesize(front, back, empty, proj, mat, {0.11f, 0.22f, 0.33f});
    const Vec3 got = miss.color[(H / 2) * W + W / 2];
    // Reflection ambient plus transmitted ambient (attenuated); both >= ambient*F.
    TASK_CHECK(got.x >= 0.f && got.y >= 0.f && got.z >= 0.f);
    TASK_CHECK(miss.stats.background_misses >= 1);
    TASK_CHECK(miss.stats.exit_hits >= 1);
    return 0;
}

int transport_invalid_arguments_rejected() {
    const int W = 4, H = 4;
    Mat4 proj = MakePerspective(-1.f, 1.f, -1.f, 1.f, 1.f, 40.f);
    DepthTarget front(W, H), back(W, H);
    BackgroundTarget bg(W, H);
    GlassMaterial bad_ior; bad_ior.ior = 3.f;
    bool threw = false;
    try { (void)Synthesize(front, back, bg, proj, bad_ior, {0.f, 0.f, 0.f}); }
    catch (const std::invalid_argument&) { threw = true; }
    TASK_CHECK(threw);

    GlassMaterial bad_sigma; bad_sigma.sigma_a = {-1.f, 0.f, 0.f};
    threw = false;
    try { (void)Synthesize(front, back, bg, proj, bad_sigma, {0.f, 0.f, 0.f}); }
    catch (const std::invalid_argument&) { threw = true; }
    TASK_CHECK(threw);

    DepthTarget wrong(W, H + 1);
    threw = false;
    try { (void)Synthesize(front, wrong, bg, proj, GlassMaterial{}, {0.f, 0.f, 0.f}); }
    catch (const std::invalid_argument&) { threw = true; }
    TASK_CHECK(threw);
    return 0;
}

int transport_repeat_synthesis_independence() {
    // Changing absorption between runs must not feed back; every output is a
    // fresh allocation and the captures stay read-only.
    const int W = 8, H = 8;
    Mat4 view = IdentityMat4();
    Mat4 normal = IdentityMat4();
    Mat4 proj = MakePerspective(-1.f, 1.f, -1.f, 1.f, 1.f, 60.f);
    auto glass = MakeBox({0.f, 0.f, -15.f}, {3.f, 3.f, 1.f});
    DepthTarget front(W, H), back(W, H);
    RasterizeAll(front, view, proj, normal, glass, FaceMode::FrontFaces);
    RasterizeAll(back, view, proj, normal, glass, FaceMode::BackFaces);
    BackgroundTarget bg(W, H);
    auto wall = MakeBox({0.f, 0.f, -30.f}, {30.f, 30.f, 1.f});
    RasterizeAll(bg, view, proj, wall, {0.5f, 0.5f, 0.5f});

    GlassMaterial a; a.ior = 1.5f; a.sigma_a = {0.f, 0.f, 0.f};
    GlassMaterial b; b.ior = 1.5f; b.sigma_a = {2.f, 2.f, 2.f};
    SynthesisResult ra = Synthesize(front, back, bg, proj, a, {0.05f, 0.05f, 0.05f});
    SynthesisResult rb = Synthesize(front, back, bg, proj, b, {0.05f, 0.05f, 0.05f});
    SynthesisResult ra2 = Synthesize(front, back, bg, proj, a, {0.05f, 0.05f, 0.05f});
    TASK_CHECK(ra.color.data() != rb.color.data());
    for (size_t i = 0; i < ra.color.size(); ++i) {
        TASK_CHECK(ApproxV(ra.color[i], ra2.color[i], 0.f));
        TASK_CHECK(rb.color[i].x <= ra.color[i].x + 1e-6f);
    }
    return 0;
}

} // namespace

extern const struct task_case task_cases[] = {
    {"capture", "pixel_conventions", capture_pixel_conventions},
    {"capture", "backface_bypass_and_interpolation", capture_backface_bypass_and_interpolation},
    {"capture", "asymmetric_and_non_square", capture_asymmetric_and_non_square},
    {"capture", "wedge_layers_and_ownership", capture_wedge_layers_and_ownership},
    {"trace", "snell_directions", trace_snell_directions},
    {"trace", "continuous_grid_intersection", trace_continuous_grid_intersection},
    {"trace", "narrow_target_and_border", trace_narrow_target_and_border},
    {"trace", "corner_crossing_visit_budget", trace_corner_crossing_visit_budget},
    {"trace", "same_depth_different_normals_fork", trace_same_depth_different_normals_fork},
    {"transport", "fresnel_and_absorption", transport_fresnel_and_absorption},
    {"transport", "total_internal_reflection", transport_total_internal_reflection},
    {"transport", "exit_missing", transport_exit_missing},
    {"transport", "background_occlusion_and_miss", transport_background_occlusion_and_miss},
    {"transport", "invalid_arguments_rejected", transport_invalid_arguments_rejected},
    {"transport", "repeat_synthesis_independence", transport_repeat_synthesis_independence}
};
extern const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
