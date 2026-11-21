// Tests for the opaque CPU surfel path.
//
// Registered through the harness in task_support/check.h as three groups:
//   footprint     - support rectangle extremal values, near clipping,
//                   non-uniform tangent half-axes, oblique views
//   reconstruction- per-pixel depth swapping, delta band, equal-depth colour
//                   symmetry, no back-layer leak, normalisation
//   integration   - tile boundaries, real triangle base occlusion, statistics
//
// The tests use an independent analytic disk/ray implementation where it
// matters, plus a couple of scenes small enough to verify by hand.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <limits>
#include <vector>

#include "src/surfels.h"
#include "task_support/check.h"

using namespace gfx;

namespace {

using D3 = std::array<double, 3>;

constexpr double kInf = std::numeric_limits<double>::infinity();

// ---------------------------------------------------------------------------
// Camera helpers (independent of the production code)
// ---------------------------------------------------------------------------

auto Perspective(double near_d, double far_d, double l, double r, double b, double t) -> Mat4d {
    Mat4d m;
    m(0, 0) = 2 * near_d / (r - l);
    m(0, 2) = (r + l) / (r - l);
    m(1, 1) = 2 * near_d / (t - b);
    m(1, 2) = (t + b) / (t - b);
    m(2, 2) = -(far_d + near_d) / (far_d - near_d);
    m(2, 3) = -2 * far_d * near_d / (far_d - near_d);
    m(3, 2) = -1;
    return m;
}

auto MakeCamera(int near_far_hint = 0) -> SurfelCamera {
    (void)near_far_hint;
    SurfelCamera cam;
    cam.view = Mat4d::Identity();
    cam.projection = Perspective(0.5, 60.0, -1.0, 1.0, -1.0, 1.0);
    cam.near_depth = 0.5;
    cam.far_depth = 60.0;
    cam.light_direction = {0.0, 0.0, -1.0};
    return cam;
}

auto MakeDisk(D3 p, D3 a, D3 b, Color c) -> Surfel {
    Surfel s;
    s.p = p;
    s.a = a;
    s.b = b;
    s.color = c;
    return s;
}

auto Base(int w, int h) -> BaseImage { return BaseImage::Empty(w, h); }

// Unproject pixel centre (x+.5, y+.5) into a view-space ray direction whose
// z component is -1, so the ray parameter is view depth.  Independent of the
// code under test.
auto PixelRay(const Mat4d& inv_projection, int w, int h, int x, int y) -> D3 {
    const double nx = 2.0 * (x + 0.5) / w - 1.0;
    const double ny = 2.0 * (y + 0.5) / h - 1.0;
    const auto v = inv_projection.Times(D3{nx, ny, 0.0}, 1.0);
    return {v[0] / v[3] / (-(v[2] / v[3])), v[1] / v[3] / (-(v[2] / v[3])), -1.0};
}

// Independent analytic disk/ray test: returns nearest valid depth > 0 or
// +infinity if the ray misses the disk [p, a, b].
auto ReferenceHit(const Surfel& s, const D3& ray, double near_d, double far_d) -> double {
    const D3 n = {
        s.a[1] * s.b[2] - s.a[2] * s.b[1],
        s.a[2] * s.b[0] - s.a[0] * s.b[2],
        s.a[0] * s.b[1] - s.a[1] * s.b[0],
    };
    const double nl = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
    if (!(nl > 1e-12)) return kInf;
    const double den = (ray[0] * n[0] + ray[1] * n[1] + ray[2] * n[2]) / nl;
    if (std::abs(den) < 1e-12) return kInf;
    const double t = (s.p[0] * n[0] + s.p[1] * n[1] + s.p[2] * n[2]) / nl / den;
    if (!(t >= near_d) || !(t <= far_d)) return kInf;
    const D3 h = {ray[0] * t, ray[1] * t, ray[2] * t};
    const D3 w = {h[0] - s.p[0], h[1] - s.p[1], h[2] - s.p[2]};
    const double aa = s.a[0] * s.a[0] + s.a[1] * s.a[1] + s.a[2] * s.a[2];
    const double bb = s.b[0] * s.b[0] + s.b[1] * s.b[1] + s.b[2] * s.b[2];
    const double ab = s.a[0] * s.b[0] + s.a[1] * s.b[1] + s.a[2] * s.b[2];
    const double wa = w[0] * s.a[0] + w[1] * s.a[1] + w[2] * s.a[2];
    const double wb = w[0] * s.b[0] + w[1] * s.b[1] + w[2] * s.b[2];
    const double det = aa * bb - ab * ab;
    if (!(std::abs(det) > 1e-18)) return kInf;
    const double u = (bb * wa - ab * wb) / det;
    const double v = (aa * wb - ab * wa) / det;
    if (u * u + v * v >= 1.0) return kInf;
    return t;
}

auto CountVisible(const SurfelResult& r) -> size_t {
    size_t n = 0;
    for (double d : r.depth) if (std::isfinite(d)) ++n;
    return n;
}

} // namespace

// ===========================================================================
// group: footprint
// ===========================================================================

namespace {

// A disk fully inside the depth range: the published continuous support must
// match the exact extremal projected coordinates, not a centre + half-axis
// approximation.  We compare against dense sampling of the disk boundary and,
// more sharply, against the analytic conic extrema recomputed independently.
int test_support_exact_extrema() {
    const int W = 200, H = 120;
    SurfelCamera cam;
    cam.view = Mat4d::Identity();
    // Deliberately asymmetric frustum.
    cam.projection = Perspective(0.7, 200.0, -1.3, 0.9, -0.6, 1.1);
    cam.near_depth = 0.7;
    cam.far_depth = 200.0;
    cam.light_direction = {0.0, 0.2, -1.0};

    // Oblique, non-uniform disk: a and b have very different lengths and are
    // not aligned with the screen axes.
    Surfel s = MakeDisk({0.4, -0.2, -6.0}, {0.7, 0.2, 0.1}, {-0.1, 0.5, 0.25},
                        Color(1, 1, 1));

    BaseImage base = Base(W, H);
    SurfelOptions opt;
    SurfelResult r;
    TASK_CHECK(RenderSurfels({s}, cam, base, opt, &r));
    TASK_CHECK(r.stats.per_point[0].support_kind == 0);

    // Exact support derived from the published values.
    const auto& pp = r.stats.per_point[0];

    // Dense boundary sampling with the independent projection.
    double x0 = kInf, x1 = -kInf, y0 = kInf, y1 = -kInf;
    const int N = 20000;
    for (int i = 0; i < N; ++i) {
        const double th = 2.0 * M_PI * i / N;
        const double u = std::cos(th), v = std::sin(th);
        const D3 q = {s.p[0] + s.a[0] * u + s.b[0] * v,
                      s.p[1] + s.a[1] * u + s.b[1] * v,
                      s.p[2] + s.a[2] * u + s.b[2] * v};
        const auto c = cam.projection.Times(q, 1.0);
        if (!(c[3] > 0)) continue;
        const double sx = (c[0] / c[3] + 1.0) * 0.5 * W;
        const double sy = (c[1] / c[3] + 1.0) * 0.5 * H;
        x0 = std::min(x0, sx); x1 = std::max(x1, sx);
        y0 = std::min(y0, sy); y1 = std::max(y1, sy);
    }
    // Sampling only approaches the true extrema from the inside.
    TASK_CHECK(pp.support_x_min <= x0 + 1e-4);
    TASK_CHECK(pp.support_x_max >= x1 - 1e-4);
    TASK_CHECK(pp.support_y_min <= y0 + 1e-4);
    TASK_CHECK(pp.support_y_max >= y1 - 1e-4);
    // ... and it is tight to better than 1e-6 relative to the analytic value.
    // Use the conic-extremum formula at 1e-9 to cross-check tightly.
    auto extremum = [&](int row, double* lo, double* hi) {
        const double A0 = cam.projection(row, 0) * s.p[0] + cam.projection(row, 1) * s.p[1] +
                          cam.projection(row, 2) * s.p[2] + cam.projection(row, 3);
        const double A1 = cam.projection(row, 0) * s.a[0] + cam.projection(row, 1) * s.a[1] +
                          cam.projection(row, 2) * s.a[2];
        const double A2 = cam.projection(row, 0) * s.b[0] + cam.projection(row, 1) * s.b[1] +
                          cam.projection(row, 2) * s.b[2];
        const double B0 = cam.projection(3, 0) * s.p[0] + cam.projection(3, 1) * s.p[1] +
                          cam.projection(3, 2) * s.p[2] + cam.projection(3, 3);
        const double B1 = cam.projection(3, 0) * s.a[0] + cam.projection(3, 1) * s.a[1] +
                          cam.projection(3, 2) * s.a[2];
        const double B2 = cam.projection(3, 0) * s.b[0] + cam.projection(3, 1) * s.b[1] +
                          cam.projection(3, 2) * s.b[2];
        const double qa = B0 * B0 - B1 * B1 - B2 * B2;
        const double qb = 2.0 * (A1 * B1 + A2 * B2 - A0 * B0);
        const double qc = A0 * A0 - A1 * A1 - A2 * A2;
        const double disc = qb * qb - 4.0 * qa * qc;
        const double sq = std::sqrt(disc > 0.0 ? disc : 0.0);
        const double r0 = (-qb - sq) / (2.0 * qa);
        const double r1 = (-qb + sq) / (2.0 * qa);
        *lo = std::min(r0, r1);
        *hi = std::max(r0, r1);
        return disc >= 0.0;
    };
    double ax0, ax1, ay0, ay1;
    TASK_CHECK(extremum(0, &ax0, &ax1));
    TASK_CHECK(extremum(1, &ay0, &ay1));
    const double e0 = std::abs((ax0 + 1.0) * 0.5 * W - pp.support_x_min);
    const double e1 = std::abs((ax1 + 1.0) * 0.5 * W - pp.support_x_max);
    const double e2 = std::abs((ay0 + 1.0) * 0.5 * H - pp.support_y_min);
    const double e3 = std::abs((ay1 + 1.0) * 0.5 * H - pp.support_y_max);
    TASK_CHECK(e0 < 1e-6);
    TASK_CHECK(e1 < 1e-6);
    TASK_CHECK(e2 < 1e-6);
    TASK_CHECK(e3 < 1e-6);
    return 0;
}

// A disk crossing the near plane: kind must be the conservative clipped-square
// bound, it must contain every ray hit found by brute force, and it must be
// finite.
int test_near_clip_support() {
    const int W = 96, H = 96;
    SurfelCamera cam = MakeCamera();
    Surfel s = MakeDisk({0.0, 0.0, -0.9}, {0.6, 0.0, 0.55}, {0.0, 0.45, 0.35},
                        Color(0, 1, 0));
    BaseImage base = Base(W, H);
    SurfelOptions opt;
    SurfelResult r;
    TASK_CHECK(RenderSurfels({s}, cam, base, opt, &r));
    TASK_CHECK(r.stats.per_point[0].support_kind == 1);
    const auto& pp = r.stats.per_point[0];
    TASK_CHECK(std::isfinite(pp.support_x_min) && std::isfinite(pp.support_x_max));
    // We cannot call the private conservative bound, but the screen bounds
    // must enclose every brute-force hit.
    const Mat4d inv = cam.projection.Inverse();
    size_t hits = 0;
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            const D3 ray = PixelRay(inv, W, H, x, y);
            if (std::isfinite(ReferenceHit(s, ray, cam.near_depth, cam.far_depth))) {
                ++hits;
                TASK_CHECK(pp.screen_bounds.Contains(x, y));
            }
        }
    TASK_CHECK(hits > 0);
    TASK_CHECK(r.stats.pixels_with_hits == hits);
    return 0;
}

// Non-uniform half-axes and oblique view: the rendered hit set matches an
// independent ray/disk reference over the entire image.
int test_nonuniform_oblique_matches_reference() {
    const int W = 80, H = 60;
    SurfelCamera cam = MakeCamera();
    Surfel s = MakeDisk({0.3, 0.1, -5.0}, {0.9, 0.05, -0.2}, {0.1, 0.35, 0.6},
                        Color(0.2, 0.7, 0.4));
    BaseImage base = Base(W, H);
    SurfelOptions opt;
    SurfelResult r;
    TASK_CHECK(RenderSurfels({s}, cam, base, opt, &r));
    const Mat4d inv = cam.projection.Inverse();
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            const double d = ReferenceHit(s, PixelRay(inv, W, H, x, y),
                                          cam.near_depth, cam.far_depth);
            const double got = r.depth[static_cast<size_t>(y) * W + x];
            if (std::isfinite(d)) {
                TASK_CHECK(std::abs(got - d) < 1e-6);
            } else {
                TASK_CHECK(!std::isfinite(got));
            }
        }
    return 0;
}

// A disk parallel to the image plane: the exact support is a scaled circle;
// the rectangle must contain every hit and stay tight.
int test_face_on_support() {
    const int W = 160, H = 160;
    SurfelCamera cam = MakeCamera();
    Surfel s = MakeDisk({0.5, -0.3, -4.0}, {0.55, 0, 0}, {0, 0.35, 0}, Color(1, 1, 1));
    BaseImage base = Base(W, H);
    SurfelResult r;
    TASK_CHECK(RenderSurfels({s}, cam, base, {}, &r));
    const auto& pp = r.stats.per_point[0];
    TASK_CHECK(pp.support_kind == 0);

    const Mat4d inv = cam.projection.Inverse();
    size_t hits = 0;
    double x0 = 1e18, x1 = -1e18, y0 = 1e18, y1 = -1e18;
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            if (std::isfinite(ReferenceHit(s, PixelRay(inv, W, H, x, y),
                                           cam.near_depth, cam.far_depth))) {
                ++hits;
                x0 = std::min(x0, (double)x); x1 = std::max(x1, (double)x);
                y0 = std::min(y0, (double)y); y1 = std::max(y1, (double)y);
                TASK_CHECK(pp.screen_bounds.Contains(x, y));
            }
        }
    TASK_CHECK(hits > 0);
    // Every hit pixel centre is inside the exact region, so the exact support
    // must reach at least to half a pixel inside the hit extremes.
    TASK_CHECK(pp.support_x_min <= x0 + 0.5 + 1e-6);
    TASK_CHECK(pp.support_x_max >= x1 - 0.5 - 1e-6);
    TASK_CHECK(pp.support_y_min <= y0 + 0.5 + 1e-6);
    TASK_CHECK(pp.support_y_max >= y1 - 0.5 - 1e-6);
    return 0;
}

// The disk centre lies outside the frustum but part of the disk is visible: it
// must not be culled, and the reported support must contain the visible pixels.
int test_centre_outside_frustum_visible() {
    const int W = 128, H = 96;
    SurfelCamera cam = MakeCamera();
    // Centre to the right of the right frustum edge (x/z ratio > 1).
    Surfel s = MakeDisk({3.2, 0, -5.0}, {1.6, 0, 0}, {0, 1.2, 0}, Color(1, 1, 1));
    BaseImage base = Base(W, H);
    SurfelResult r;
    TASK_CHECK(RenderSurfels({s}, cam, base, {}, &r));
    const auto& pp = r.stats.per_point[0];

    const Mat4d inv = cam.projection.Inverse();
    size_t hits = 0;
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            if (std::isfinite(ReferenceHit(s, PixelRay(inv, W, H, x, y),
                                           cam.near_depth, cam.far_depth))) {
                ++hits;
                TASK_CHECK(pp.screen_bounds.Contains(x, y));
            }
        }
    TASK_CHECK(hits > 0);
    TASK_CHECK(r.stats.pixels_with_hits == hits);
    return 0;
}

} // namespace

// ===========================================================================
// group: reconstruction
// ===========================================================================

namespace {

// Two tilted disks that swap depth across the screen: per pixel the nearer
// one must win, independent of input order.
int test_per_pixel_depth_swap() {
    const int W = 96, H = 96;
    SurfelCamera cam = MakeCamera();
    BaseImage base = Base(W, H);
    // Both centres at depth 6.  A's +a tilt has +z, so its right half (world +x,
    // which is screen +x) is nearer; B's tilt is opposite, so B is nearer on
    // the left.  The winner therefore swaps across the screen.
    Surfel A = MakeDisk({0, 0, -6}, {1.5, 0, 0.9}, {0, 1.5, 0}, Color(1, 0, 0));
    Surfel B = MakeDisk({0, 0, -6}, {1.5, 0, -0.9}, {0, 1.5, 0}, Color(0, 0, 1));

    SurfelResult ra, rb, ab, ba;
    TASK_CHECK(RenderSurfels({A}, cam, base, {}, &ra));
    TASK_CHECK(RenderSurfels({B}, cam, base, {}, &rb));
    TASK_CHECK(RenderSurfels({A, B}, cam, base, {}, &ab));
    TASK_CHECK(RenderSurfels({B, A}, cam, base, {}, &ba));

    int both = 0, right_red = 0, left_blue = 0;
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            const size_t i = static_cast<size_t>(y) * W + x;
            const bool ha = std::isfinite(ra.depth[i]), hb = std::isfinite(rb.depth[i]);
            if (ha && hb) {
                ++both;
                const double near_d = std::min(ra.depth[i], rb.depth[i]);
                TASK_CHECK(std::abs(ab.depth[i] - near_d) < 1e-9);
                if (ra.depth[i] < rb.depth[i]) {
                    TASK_CHECK(std::abs(ab.color[i].r - ra.color[i].r) < 1e-5);
                    if (x >= W / 2) ++right_red;
                } else {
                    TASK_CHECK(std::abs(ab.color[i].b - rb.color[i].b) < 1e-5);
                    if (x < W / 2) ++left_blue;
                }
            } else {
                const auto& solo = ha ? ra : rb;
                if (ha || hb) TASK_CHECK(std::abs(ab.depth[i] - solo.depth[i]) < 1e-9);
            }
            // Order symmetric.
            TASK_CHECK(std::abs(ab.color[i].r - ba.color[i].r) < 1e-6);
            TASK_CHECK(std::abs(ab.color[i].b - ba.color[i].b) < 1e-6);
        }
    TASK_CHECK(both > 0);
    TASK_CHECK(right_red > 0);
    TASK_CHECK(left_blue > 0);
    return 0;
}

// The delta band is inclusive: a hit exactly delta behind the nearest one is
// blended; one just outside is not.  Colour is the normalised average, not an
// alpha composite.
int test_delta_band_boundary() {
    const int W = 32, H = 32;
    SurfelCamera cam = MakeCamera();
    BaseImage base = Base(W, H);
    Surfel A = MakeDisk({0, 0, -5}, {2, 0, 0}, {0, 2, 0}, Color(1, 0, 0));
    Surfel B = MakeDisk({0, 0, -6}, {2, 0, 0}, {0, 2, 0}, Color(0, 1, 0));
    const size_t centre = static_cast<size_t>(H / 2) * W + W / 2;

    SurfelResult just_out, exact, well_inside;
    TASK_CHECK(RenderSurfels({A, B}, cam, base, SurfelOptions{0.999, 16}, &just_out));
    TASK_CHECK(RenderSurfels({A, B}, cam, base, SurfelOptions{1.0, 16}, &exact));
    TASK_CHECK(RenderSurfels({A, B}, cam, base, SurfelOptions{2.0, 16}, &well_inside));

    // Depth gap is exactly 1.0.
    TASK_CHECK(std::abs(just_out.depth[centre] - 5.0) < 1e-9);
    TASK_CHECK(just_out.color[centre].g < 1e-6);  // only the front disk

    TASK_CHECK(std::abs(exact.depth[centre] - 5.0) < 1e-9);
    TASK_CHECK(exact.color[centre].r > 1e-3);
    TASK_CHECK(exact.color[centre].g > 1e-3);
    // Equal kernel weights here -> each channel is halved (normalised), which
    // is *not* the same as a transparency composite (that would leave r ~1).
    TASK_CHECK(std::abs(exact.color[centre].r - well_inside.color[centre].r) < 1e-6);
    TASK_CHECK(exact.color[centre].r < 0.6);

    // Output depth is the nearest depth, never an average.
    TASK_CHECK(std::abs(well_inside.depth[centre] - 5.0) < 1e-9);
    return 0;
}

// Equal-depth surfels with different colours must produce the same colour in
// both input orders (no order-based tie-breaking), and must blend all of them.
int test_equal_depth_colour_symmetry() {
    const int W = 64, H = 32;
    SurfelCamera cam = MakeCamera();
    BaseImage base = Base(W, H);
    Surfel A = MakeDisk({-0.3, 0, -4}, {0.6, 0, 0}, {0, 0.6, 0}, Color(1, 0, 0));
    Surfel B = MakeDisk({0.3, 0, -4}, {0.6, 0, 0}, {0, 0.6, 0}, Color(0, 0, 1));
    SurfelResult r1, r2;
    TASK_CHECK(RenderSurfels({A, B}, cam, base, {}, &r1));
    TASK_CHECK(RenderSurfels({B, A}, cam, base, {}, &r2));

    int overlap = 0;
    for (size_t i = 0; i < r1.color.size(); ++i) {
        TASK_CHECK(std::abs(r1.color[i].r - r2.color[i].r) < 1e-6);
        TASK_CHECK(std::abs(r1.color[i].g - r2.color[i].g) < 1e-6);
        TASK_CHECK(std::abs(r1.color[i].b - r2.color[i].b) < 1e-6);
        // Background pixels both hold +infinity; treat that as equal.
        TASK_CHECK((std::isinf(r1.depth[i]) && std::isinf(r2.depth[i])) ||
                   std::abs(r1.depth[i] - r2.depth[i]) < 1e-9);
        if (r1.color[i].r > 0.01 && r1.color[i].b > 0.01) ++overlap;
    }
    TASK_CHECK(overlap > 0);
    return 0;
}

// A surfel strictly behind an opaque surfel must never leak colour, and the
// output depth is the front one.
int test_back_layer_no_leak() {
    const int W = 48, H = 48;
    SurfelCamera cam = MakeCamera();
    BaseImage base = Base(W, H);
    Surfel front = MakeDisk({0, 0, -5}, {2, 0, 0}, {0, 2, 0}, Color(1, 0, 0));
    Surfel back = MakeDisk({0, 0, -6}, {2, 0, 0}, {0, 2, 0}, Color(0, 0, 1));
    SurfelResult r;
    TASK_CHECK(RenderSurfels({front, back}, cam, base, {}, &r));
    size_t hits = 0;
    for (size_t i = 0; i < r.color.size(); ++i) {
        if (std::isfinite(r.depth[i])) {
            ++hits;
            TASK_CHECK(r.color[i].b < 1e-6);
            TASK_CHECK(std::abs(r.depth[i] - 5.0) < 1e-9);
        }
    }
    TASK_CHECK(hits > 0);
    return 0;
}

// Surfel contributions are weighted by (1 - q)^2 and normalised, so a single
// isolated surfel reproduces exactly color * shader result.
int test_kernel_normalisation() {
    const int W = 40, H = 40;
    SurfelCamera cam = MakeCamera();
    BaseImage base = Base(W, H);
    Surfel s = MakeDisk({0, 0, -4}, {1.2, 0, 0}, {0, 1.2, 0}, Color(0.3f, 0.5f, 0.9f));
    SurfelResult r;
    TASK_CHECK(RenderSurfels({s}, cam, base, {}, &r));

    // Reconstruct the shader value at the centre pixel independently.
    int cx = W / 2, cy = H / 2;
    const Mat4d inv = cam.projection.Inverse();
    const D3 ray = PixelRay(inv, W, H, cx, cy);
    const double d = ReferenceHit(s, ray, cam.near_depth, cam.far_depth);
    TASK_CHECK(std::isfinite(d));
    // View-space hit point and eye vector.
    const D3 hp = {ray[0] * d, ray[1] * d, ray[2] * d};
    double el = std::sqrt(hp[0] * hp[0] + hp[1] * hp[1] + hp[2] * hp[2]);
    Vec vdir({static_cast<float>(-hp[0] / el), static_cast<float>(-hp[1] / el),
              static_cast<float>(-hp[2] / el), 0});
    D3 n = {s.a[1] * s.b[2] - s.a[2] * s.b[1], s.a[2] * s.b[0] - s.a[0] * s.b[2],
            s.a[0] * s.b[1] - s.a[1] * s.b[0]};
    double nl = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
    n = {n[0] / nl, n[1] / nl, n[2] / nl};
    // Flip towards eye.
    const double ndv = n[0] * vdir[0] + n[1] * vdir[1] + n[2] * vdir[2];
    if (ndv < 0) n = {-n[0], -n[1], -n[2]};
    Vec nv({static_cast<float>(n[0]), static_cast<float>(n[1]), static_cast<float>(n[2]), 0});
    Vec lv({0, 0, -1, 0});
    Vec shaded = FragmentShader(nv, lv, vdir);

    const size_t i = static_cast<size_t>(cy) * W + cx;
    TASK_CHECK(std::abs(r.color[i].r - 0.3f * shaded[0]) < 1e-5);
    TASK_CHECK(std::abs(r.color[i].g - 0.5f * shaded[1]) < 1e-5);
    TASK_CHECK(std::abs(r.color[i].b - 0.9f * shaded[2]) < 1e-5);
    return 0;
}

// No hit at all keeps the background both in colour and in depth.
int test_no_hit_preserves_background() {
    const int W = 24, H = 24;
    SurfelCamera cam = MakeCamera();
    BaseImage base = Base(W, H);
    for (size_t i = 0; i < base.color.size(); ++i) {
        base.color[i] = Color(0.11f, 0.22f, 0.33f);
        base.depth[i] = 12.5;
    }
    Surfel behind = MakeDisk({0, 0, -20}, {1, 0, 0}, {0, 1, 0}, Color(1, 1, 1));
    SurfelResult r;
    TASK_CHECK(RenderSurfels({behind}, cam, base, {}, &r));
    for (size_t i = 0; i < r.color.size(); ++i) {
        TASK_CHECK(std::abs(r.color[i].r - 0.11f) < 1e-7);
        TASK_CHECK(std::abs(r.depth[i] - 12.5) < 1e-9);
    }
    return 0;
}

} // namespace

// ===========================================================================
// group: integration
// ===========================================================================

namespace {

// Small disks placed on a grid that straddle tile boundaries: every one must
// be found, and no pixel may be missed at a tile seam.
int test_tile_boundaries() {
    const int W = 64, H = 64;
    SurfelCamera cam = MakeCamera();
    BaseImage base = Base(W, H);
    std::vector<Surfel> disks;
    for (int gy = 0; gy < 6; ++gy)
        for (int gx = 0; gx < 6; ++gx) {
            const double x = -1.5 + 0.6 * gx;
            const double y = -1.5 + 0.6 * gy;
            disks.push_back(MakeDisk({x, y, -4}, {0.06, 0, 0}, {0, 0.06, 0},
                                     Color(1, 1, 1)));
        }
    SurfelResult r;
    TASK_CHECK(RenderSurfels(disks, cam, base, SurfelOptions{0.0, 16}, &r));

    const Mat4d inv = cam.projection.Inverse();
    size_t ref_hits = 0;
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            const D3 ray = PixelRay(inv, W, H, x, y);
            bool any = false;
            for (const auto& d : disks)
                if (std::isfinite(ReferenceHit(d, ray, cam.near_depth, cam.far_depth))) any = true;
            if (any) {
                ++ref_hits;
                TASK_CHECK(std::isfinite(r.depth[static_cast<size_t>(y) * W + x]));
            }
        }
    TASK_CHECK(ref_hits > 0);
    TASK_CHECK(r.stats.pixels_with_hits == ref_hits);
    TASK_CHECK(r.stats.tile_size == 16);
    TASK_CHECK(r.stats.tile_cols == 4);
    TASK_CHECK(r.stats.tile_rows == 4);
    return 0;
}

// A real triangle pass (shared FragmentShader through RasterizeTriangle) forms
// the base image; surfels in front must occlude it, surfels behind must not.
int test_triangle_base_occlusion() {
    const int W = 64, H = 64;
    SurfelCamera cam = MakeCamera();
    Target target(W, H);
    target.Reset();

    // Large triangle at view depth 8, facing the camera.
    const Vec n({0, 0, -1, 0});
    RasterizeTriangle(cam.projection, cam.view, Mat4d::Identity(),
                      Vec({-6, -6, -8, 1}), Vec({6, -6, -8, 1}), Vec({0, 6, -8, 1}),
                      n, n, n, &target);

    BaseImage base = BaseFromTarget(target);
    size_t base_hits = 0;
    const size_t centre = static_cast<size_t>(H / 2) * W + W / 2;
    for (size_t i = 0; i < base.depth.size(); ++i) {
        if (std::isfinite(base.depth[i])) {
            ++base_hits;
            TASK_CHECK(std::abs(base.depth[i] - 8.0) < 1e-5);
        }
    }
    TASK_CHECK(base_hits > 0);
    TASK_CHECK(std::isfinite(base.depth[centre]));

    // In front: surface at depth 5 wins.
    Surfel front = MakeDisk({0, 0, -5}, {2, 0, 0}, {0, 2, 0}, Color(1, 0, 0));
    SurfelResult rf;
    TASK_CHECK(RenderSurfels({front}, cam, base, {}, &rf));
    TASK_CHECK(std::abs(rf.depth[centre] - 5.0) < 1e-6);
    TASK_CHECK(rf.color[centre].r > 0.01);

    // Behind: the triangle's colour and depth are preserved exactly.
    Surfel back = MakeDisk({0, 0, -20}, {3, 0, 0}, {0, 3, 0}, Color(0, 1, 0));
    SurfelResult rb;
    TASK_CHECK(RenderSurfels({back}, cam, base, {}, &rb));
    TASK_CHECK(std::abs(rb.depth[centre] - base.depth[centre]) < 1e-12);
    TASK_CHECK(std::abs(rb.color[centre].r - base.color[centre].r) < 1e-7);
    TASK_CHECK(std::abs(rb.color[centre].g - base.color[centre].g) < 1e-7);
    return 0;
}

// Statistics must reflect real per-tile candidate work, not a full-screen
// scan: intersection_tests must be far below surfels * pixels, the counts must
// be self-consistent, and per-point tile_refs must match the tile table.
int test_statistics_are_real() {
    const int W = 128, H = 96;
    SurfelCamera cam = MakeCamera();
    BaseImage base = Base(W, H);
    std::vector<Surfel> disks;
    // Moderate-size disks spread across the image: each covers a real but
    // bounded set of pixels, so a full-screen scan would be far more work.
    for (int i = 0; i < 20; ++i)
        disks.push_back(MakeDisk({-1.4 + 0.15 * i, 0, -4}, {0.18, 0, 0}, {0, 0.18, 0},
                                 Color(1, 1, 1)));
    SurfelResult r;
    TASK_CHECK(RenderSurfels(disks, cam, base, SurfelOptions{0.0, 16}, &r));

    size_t tests = 0, refs = 0;
    for (const auto& pp : r.stats.per_point) {
        tests += pp.intersection_tests;
        refs += pp.tile_refs;
    }
    TASK_CHECK(r.stats.intersection_tests == tests);
    TASK_CHECK(tests > 0);
    TASK_CHECK(r.stats.accepted_hits > 0);
    TASK_CHECK(r.stats.pixels_with_hits > 0);
    // Full-screen brute force would be 20 * 128 * 96 = 245760.
    TASK_CHECK(tests < 20ull * W * H / 8);
    // Accepted hits are a strict subset of performed tests.
    TASK_CHECK(r.stats.accepted_hits <= r.stats.intersection_tests);

    // Recompute the reference count from the public tile table.  Each tile
    // holds at most one reference per surfel.
    size_t table_refs = 0;
    for (const auto& row : r.stats.tile_refs) {
        table_refs += row.size();
        for (size_t a = 1; a < row.size(); ++a) TASK_CHECK(row[a - 1] < row[a]);
    }
    TASK_CHECK(table_refs == refs);

    // Candidates are exactly the referenced surfels.
    std::vector<bool> seen(disks.size(), false);
    for (const auto& row : r.stats.tile_refs)
        for (int idx : row) seen[static_cast<size_t>(idx)] = true;
    std::vector<int> expected;
    for (size_t i = 0; i < seen.size(); ++i)
        if (seen[i]) expected.push_back(static_cast<int>(i));
    TASK_CHECK(r.stats.candidates == expected);
    return 0;
}

// Inputs, repeated calls, interleaved instances and freed source data must not
// change results or mutate the inputs.
int test_determinism_and_immutability() {
    const int W = 48, H = 48;
    SurfelCamera cam = MakeCamera();
    BaseImage base = Base(W, H);
    base.color[0] = Color(0.4f, 0.5f, 0.6f);
    base.depth[0] = 30.0;
    BaseImage base_copy = base;
    std::vector<Surfel> disks = {
        MakeDisk({-0.4, 0, -5}, {0.8, 0, 0.2}, {0, 0.8, 0}, Color(1, 0.5, 0)),
        MakeDisk({0.4, 0, -5.5}, {0.8, 0, -0.2}, {0, 0.8, 0}, Color(0, 0.5, 1)),
    };
    std::vector<Surfel> disks_copy = disks;

    SurfelResult a, b;
    TASK_CHECK(RenderSurfels(disks, cam, base, {}, &a));
    // Interleave with another instance.
    SurfelResult other;
    TASK_CHECK(RenderSurfels({MakeDisk({0, 0, -7}, {1, 0, 0}, {0, 1, 0}, Color(1, 1, 1))},
                             cam, base, {}, &other));
    TASK_CHECK(RenderSurfels(disks, cam, base, {}, &b));

    TASK_CHECK(a.color == b.color);
    TASK_CHECK(a.depth == b.depth);
    TASK_CHECK(a.stats.intersection_tests == b.stats.intersection_tests);

    // Inputs untouched.
    TASK_CHECK(base.color == base_copy.color);
    TASK_CHECK(base.depth == base_copy.depth);
    for (size_t i = 0; i < disks.size(); ++i) {
        TASK_CHECK(disks[i].p == disks_copy[i].p);
        TASK_CHECK(disks[i].a == disks_copy[i].a);
        TASK_CHECK(disks[i].b == disks_copy[i].b);
        TASK_CHECK(disks[i].color.r == disks_copy[i].color.r);
    }
    return 0;
}

// Invalid inputs must be rejected before any result is published, leaving the
// previous valid result intact.
int test_validation_preserves_last_result() {
    const int W = 32, H = 32;
    SurfelCamera cam = MakeCamera();
    BaseImage base = Base(W, H);
    Surfel good = MakeDisk({0, 0, -5}, {1, 0, 0}, {0, 1, 0}, Color(1, 1, 1));

    SurfelResult keep;
    TASK_CHECK(RenderSurfels({good}, cam, base, {}, &keep));
    SurfelResult sentinel = keep;

    auto expect_reject = [&](const std::vector<Surfel>& disks, const SurfelCamera& c,
                             const BaseImage& bi, const SurfelOptions& o) -> bool {
        SurfelResult r = sentinel;  // must remain untouched
        const bool ok = RenderSurfels(disks, c, bi, o, &r);
        return !ok && r.color == sentinel.color && r.depth == sentinel.depth;
    };

    // Collinear tangent half-axes.
    {
        Surfel s = MakeDisk({0, 0, -5}, {1, 1, 0}, {2, 2, 0}, Color(1, 1, 1));
        TASK_CHECK(expect_reject({s}, cam, base, {}));
    }
    // Zero-length half-axis.
    {
        Surfel s = MakeDisk({0, 0, -5}, {0, 0, 0}, {0, 1, 0}, Color(1, 1, 1));
        TASK_CHECK(expect_reject({s}, cam, base, {}));
    }
    // Non-finite centre.
    {
        Surfel s = MakeDisk({std::nan(""), 0, -5}, {1, 0, 0}, {0, 1, 0}, Color(1, 1, 1));
        TASK_CHECK(expect_reject({s}, cam, base, {}));
    }
    // Colour outside [0,1].
    {
        Surfel s = MakeDisk({0, 0, -5}, {1, 0, 0}, {0, 1, 0}, Color(1.5f, 0, 0));
        TASK_CHECK(expect_reject({s}, cam, base, {}));
    }
    // Negative delta.
    TASK_CHECK(expect_reject({good}, cam, base, SurfelOptions{-0.1, 16}));
    // Bad tile size.
    TASK_CHECK(expect_reject({good}, cam, base, SurfelOptions{0.0, 0}));

    // Invalid camera parameters.
    {
        SurfelCamera c = cam;
        c.near_depth = -1.0;
        TASK_CHECK(expect_reject({good}, c, base, {}));
    }
    {
        SurfelCamera c = cam;
        c.far_depth = c.near_depth;
        TASK_CHECK(expect_reject({good}, c, base, {}));
    }
    {
        SurfelCamera c = cam;
        c.light_direction = {0, 0, 0};
        TASK_CHECK(expect_reject({good}, c, base, {}));
    }
    {
        SurfelCamera c = cam;
        c.view(0, 0) = 2.0;  // not rigid
        TASK_CHECK(expect_reject({good}, c, base, {}));
    }
    {
        SurfelCamera c = cam;
        c.projection(3, 2) = 1.0;  // clip.w != -view.z
        TASK_CHECK(expect_reject({good}, c, base, {}));
    }
    {
        SurfelCamera c = cam;
        c.projection(0, 0) = std::nan("");
        TASK_CHECK(expect_reject({good}, c, base, {}));
    }
    // Invalid base image sizes / non-finite data.
    {
        BaseImage bad = base;
        bad.depth[0] = std::nan("");
        TASK_CHECK(expect_reject({good}, cam, bad, {}));
    }
    {
        BaseImage bad = base;
        bad.color.resize(10);
        TASK_CHECK(expect_reject({good}, cam, bad, {}));
    }
    {
        BaseImage bad = BaseImage::Empty(0, 0);
        TASK_CHECK(expect_reject({good}, cam, bad, {}));
    }
    // Non-finite delta.
    TASK_CHECK(expect_reject({good}, cam, base, SurfelOptions{std::numeric_limits<double>::infinity(), 16}));

    return 0;
}

    // Rays nearly parallel to a disk must not create spurious hits.
int test_grazing_ray_no_spurious_hits() {
    const int W = 64, H = 64;
    SurfelCamera cam = MakeCamera();
    BaseImage base = Base(W, H);
    // Disk almost edge-on: its plane is nearly parallel to the view direction.
    // a is in the screen plane, b is almost along the view direction, giving a
    // normal nearly perpendicular to the camera rays.  Any accepted hit must
    // be finite and inside the disk; near-parallel rays must not blow up.
    Surfel s = MakeDisk({0, 0, -5}, {1.5, 0, 0}, {0, 0.0015, -1.0}, Color(1, 1, 1));
    SurfelResult r;
    TASK_CHECK(RenderSurfels({s}, cam, base, {}, &r));

    const Mat4d inv = cam.projection.Inverse();
    size_t ref_hits = 0;
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            const double d = ReferenceHit(s, PixelRay(inv, W, H, x, y),
                                          cam.near_depth, cam.far_depth);
            const double got = r.depth[static_cast<size_t>(y) * W + x];
            if (std::isfinite(d)) ++ref_hits;
            // Exact agreement: no phantom hits from near-parallel rays.
            TASK_CHECK(std::isfinite(d) == std::isfinite(got));
            if (std::isfinite(got)) TASK_CHECK(std::isfinite(d));
        }
    TASK_CHECK(ref_hits == r.stats.pixels_with_hits);
    return 0;
}

} // namespace

// ===========================================================================
// Registration
// ===========================================================================

extern const struct task_case task_cases[];
extern const size_t task_case_count;

const struct task_case task_cases[] = {
    {"footprint", "support_exact_extrema", test_support_exact_extrema},
    {"footprint", "near_clip_support", test_near_clip_support},
    {"footprint", "nonuniform_oblique_reference", test_nonuniform_oblique_matches_reference},
    {"footprint", "face_on_support", test_face_on_support},
    {"footprint", "centre_outside_frustum", test_centre_outside_frustum_visible},
    {"reconstruction", "per_pixel_depth_swap", test_per_pixel_depth_swap},
    {"reconstruction", "delta_band_boundary", test_delta_band_boundary},
    {"reconstruction", "equal_depth_symmetry", test_equal_depth_colour_symmetry},
    {"reconstruction", "back_layer_no_leak", test_back_layer_no_leak},
    {"reconstruction", "kernel_normalisation", test_kernel_normalisation},
    {"reconstruction", "no_hit_preserves_background", test_no_hit_preserves_background},
    {"integration", "tile_boundaries", test_tile_boundaries},
    {"integration", "triangle_base_occlusion", test_triangle_base_occlusion},
    {"integration", "statistics_are_real", test_statistics_are_real},
    {"integration", "determinism_and_immutability", test_determinism_and_immutability},
    {"integration", "validation_preserves_last_result", test_validation_preserves_last_result},
    {"integration", "grazing_ray_no_spurious_hits", test_grazing_ray_no_spurious_hits},
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
