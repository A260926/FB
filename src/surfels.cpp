#include "surfels.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <limits>
#include <numeric>
#include <string>
#include <tuple>

namespace gfx {
namespace {

using D3 = std::array<double, 3>;

inline auto Add(const D3& a, const D3& b) -> D3 { return {a[0] + b[0], a[1] + b[1], a[2] + b[2]}; }
inline auto Sub(const D3& a, const D3& b) -> D3 { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
inline auto Mul(const D3& a, double s) -> D3 { return {a[0] * s, a[1] * s, a[2] * s}; }
inline auto Dot(const D3& a, const D3& b) -> double { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
inline auto Cross(const D3& a, const D3& b) -> D3 {
    return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}
inline auto Len(const D3& a) -> double { return std::sqrt(Dot(a, a)); }
inline auto Normalize(const D3& a) -> D3 {
    const double l = Len(a);
    return l > 0 ? Mul(a, 1.0 / l) : D3{0, 0, 0};
}
inline auto Finite(double x) -> bool { return std::isfinite(x); }
inline auto Finite(const D3& a) -> bool { return Finite(a[0]) && Finite(a[1]) && Finite(a[2]); }
inline auto Finite(const Color& c) -> bool { return Finite(c.r) && Finite(c.g) && Finite(c.b); }
inline auto ValidColor(const Color& c) -> bool {
    return Finite(c) && c.r >= 0 && c.r <= 1 && c.g >= 0 && c.g <= 1 && c.b >= 0 && c.b <= 1;
}

auto FiniteMatrix(const Mat4d& m) -> bool {
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            if (!Finite(m(i, j))) return false;
    return true;
}

auto IsRigid(const Mat4d& m) -> bool {
    // Rotation (+ optional reflection) plus translation: linear part
    // orthonormal, bottom row (0,0,0,1).
    if (std::abs(m(3, 0)) > 1e-12 || std::abs(m(3, 1)) > 1e-12 ||
        std::abs(m(3, 2)) > 1e-12 || std::abs(m(3, 3) - 1.0) > 1e-12)
        return false;
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) {
            double s = 0;
            for (int k = 0; k < 3; ++k) s += m(k, i) * m(k, j);
            const double want = (i == j) ? 1.0 : 0.0;
            if (std::abs(s - want) > 1e-9) return false;
        }
    return true;
}

/* ------------------------------------------------------------------------- */
/* 3D polygon clipping                                                       */
/* ------------------------------------------------------------------------- */

using Poly = std::vector<D3>;

// Clip a 3D polygon by the half-space g(x) >= 0.
template <typename G>
auto ClipHalfSpace(const Poly& poly, G g) -> Poly {
    Poly out;
    const size_t n = poly.size();
    if (n == 0) return out;
    for (size_t i = 0; i < n; ++i) {
        const D3& a = poly[i];
        const D3& b = poly[(i + 1) % n];
        const double ga = g(a), gb = g(b);
        if (ga >= 0) out.push_back(a);
        if ((ga >= 0) != (gb >= 0)) {
            const double denom = ga - gb;
            if (std::abs(denom) > 1e-300) {
                const double t = ga / denom;
                out.push_back(Add(a, Mul(Sub(b, a), t)));
            }
        }
    }
    return out;
}

/* ------------------------------------------------------------------------- */
/* Per-surfel view-space data + support bounds                               */
/* ------------------------------------------------------------------------- */

struct ViewSurfel {
    D3 p{}, a{}, b{}, n{};
    double nn = 0;        // |a x b|^2
    double depth_min = 0; // min view depth over the disk
    double depth_max = 0; // max view depth over the disk
};

struct SupportBounds {
    int kind = 2;          // 0 exact, 1 conservative, 3 outside depth range
    bool valid = false;
    double x_min = 0, x_max = 0, y_min = 0, y_max = 0;  // continuous screen coords
};

// Row of a 4x4 matrix applied to a view-space point (x, y, z, 1).
struct RowForm {
    D3 q;
    double c;
    auto Eval(const D3& x) const -> double { return Dot(q, x) + c; }
};

auto MakeRow(const Mat4d& m, int i) -> RowForm {
    return RowForm{{m(i, 0), m(i, 1), m(i, 2)}, m(i, 3)};
}

// Exact extremal value of (A0 + A1 u + A2 v) / (B0 + B1 u + B2 v) over the
// unit disk.  Valid when the denominator stays strictly positive (guaranteed
// when the whole disk is inside the near/far slab).
auto ExtremalRatio(const RowForm& num, const RowForm& den, const ViewSurfel& s,
                   double* out_min, double* out_max) -> bool {
    const double A0 = num.Eval(s.p);
    const double A1 = Dot(num.q, s.a);
    const double A2 = Dot(num.q, s.b);
    const double B0 = den.Eval(s.p);
    const double B1 = Dot(den.q, s.a);
    const double B2 = Dot(den.q, s.b);

    // Tangency of the level set f = lambda to the unit circle:
    //   (A0 - lambda B0)^2 = (A1 - lambda B1)^2 + (A2 - lambda B2)^2.
    const double qa = B0 * B0 - B1 * B1 - B2 * B2;
    const double qb = 2.0 * (A1 * B1 + A2 * B2 - A0 * B0);
    const double qc = A0 * A0 - A1 * A1 - A2 * A2;
    if (std::abs(qa) < 1e-18) {
        // Degenerate: the level sets are parallel lines; solve qb*lambda+qc=0.
        if (std::abs(qb) < 1e-300) return false;
        const double r = -qc / qb;
        if (!std::isfinite(r)) return false;
        *out_min = *out_max = r;
        return true;
    }

    const double disc = qb * qb - 4.0 * qa * qc;
    if (disc < 0) {
        if (disc > -1e-12 * (qb * qb + std::abs(4.0 * qa * qc))) {
            const double r = -qb / (2.0 * qa);
            *out_min = *out_max = r;
            return std::isfinite(r);
        }
        return false;
    }
    const double sq = std::sqrt(disc);
    // Stable roots.
    const double q = -0.5 * (qb + (qb >= 0 ? sq : -sq));
    double r0, r1;
    if (q == 0) {
        r0 = r1 = 0;
    } else {
        r0 = q / qa;
        r1 = qc / q;
    }
    if (r0 > r1) std::swap(r0, r1);
    if (!std::isfinite(r0) || !std::isfinite(r1)) return false;
    *out_min = r0;
    *out_max = r1;
    return true;
}

// Compute conservative support by clipping the tangent square (u,v in
// [-1,1]) to the near/far slab and projecting its corners.
auto ConservativeSupport(const Mat4d& projection, int width, int height,
                         const ViewSurfel& s, double near_d, double far_d,
                         SupportBounds* out) -> bool {
    Poly square;
    square.reserve(4);
    square.push_back(Add(s.p, Add(s.a, s.b)));
    square.push_back(Add(s.p, Add(Mul(s.a, -1.0), s.b)));
    square.push_back(Add(s.p, Add(Mul(s.a, -1.0), Mul(s.b, -1.0))));
    square.push_back(Add(s.p, Add(s.a, Mul(s.b, -1.0))));

    auto clipped = ClipHalfSpace(square, [&](const D3& x) { return -x[2] - near_d; });
    if (clipped.empty()) return false;
    clipped = ClipHalfSpace(clipped, [&](const D3& x) { return x[2] + far_d; });
    if (clipped.empty()) return false;

    double xmin = std::numeric_limits<double>::infinity(), xmax = -xmin;
    double ymin = xmin, ymax = xmax;
    bool any = false;
    for (const auto& x : clipped) {
        const auto clip = projection.Times(x, 1.0);
        if (!(clip[3] > 1e-12)) continue;
        const double nx = clip[0] / clip[3];
        const double ny = clip[1] / clip[3];
        const double sx = (nx + 1.0) * 0.5 * width;
        const double sy = (ny + 1.0) * 0.5 * height;
        if (!std::isfinite(sx) || !std::isfinite(sy)) return false;
        xmin = std::min(xmin, sx);
        xmax = std::max(xmax, sx);
        ymin = std::min(ymin, sy);
        ymax = std::max(ymax, sy);
        any = true;
    }
    if (!any) return false;
    // The clipped square touches its own boundary, so margin of one pixel is
    // more than enough to stay conservative for the contained disk.
    out->x_min = xmin - 1.0;
    out->x_max = xmax + 1.0;
    out->y_min = ymin - 1.0;
    out->y_max = ymax + 1.0;
    out->kind = 1;
    out->valid = true;
    return true;
}

auto ComputeSupport(const Mat4d& projection, int width, int height,
                    double near_d, double far_d, const ViewSurfel& s,
                    SupportBounds* out) -> void {
    out->valid = false;
    out->kind = 3;
    if (s.depth_max < near_d || s.depth_min > far_d) return;

    const RowForm row_x = MakeRow(projection, 0);
    const RowForm row_y = MakeRow(projection, 1);
    const RowForm row_w = MakeRow(projection, 3);

    const bool inside_slab = (s.depth_min >= near_d && s.depth_max <= far_d);
    if (inside_slab) {
        double x0, x1, y0, y1;
        if (ExtremalRatio(row_x, row_w, s, &x0, &x1) &&
            ExtremalRatio(row_y, row_w, s, &y0, &y1)) {
            const double sx0 = (x0 + 1.0) * 0.5 * width;
            const double sx1 = (x1 + 1.0) * 0.5 * width;
            const double sy0 = (y0 + 1.0) * 0.5 * height;
            const double sy1 = (y1 + 1.0) * 0.5 * height;
            if (std::isfinite(sx0) && std::isfinite(sx1) &&
                std::isfinite(sy0) && std::isfinite(sy1)) {
                out->x_min = std::min(sx0, sx1);
                out->x_max = std::max(sx0, sx1);
                out->y_min = std::min(sy0, sy1);
                out->y_max = std::max(sy0, sy1);
                out->kind = 0;
                out->valid = true;
                return;
            }
        }
    }
    ConservativeSupport(projection, width, height, s, near_d, far_d, out);
}

/* ------------------------------------------------------------------------- */
/* Ray / disk intersection                                                   */
/* ------------------------------------------------------------------------- */

struct RayDir {
    bool ok = false;
    D3 d{};  // direction with d.z == -1, so the parameter equals view depth
};

auto MakeRay(const Mat4d& inv_projection, int width, int height, int x, int y) -> RayDir {
    RayDir r;
    const double nx = 2.0 * (x + 0.5) / width - 1.0;
    const double ny = 2.0 * (y + 0.5) / height - 1.0;
    // Unproject the pixel centre at NDC z = 0 (any z on the ray works).
    const auto h = inv_projection.Times(D3{nx, ny, 0.0}, 1.0);
    if (!(std::abs(h[3]) > 1e-300)) return r;
    const double px = h[0] / h[3];
    const double py = h[1] / h[3];
    const double pz = h[2] / h[3];
    if (!(pz < 0)) return r;  // ray must point into the scene
    // Normalise the view-space direction so the ray parameter is view depth:
    // d = P / (-P.z) has d.z = -1, hence P(t) = t*d gives depth t.
    r.d = {px / -pz, py / -pz, -1.0};
    r.ok = std::isfinite(r.d[0]) && std::isfinite(r.d[1]);
    return r;
}

struct Hit {
    double depth;
    double u, v, q;
    int surfel;
};

struct HitKeyLess {
    const std::vector<Surfel>* surfels;
    auto operator()(const Hit& a, const Hit& b) const -> bool {
        if (a.depth != b.depth) return a.depth < b.depth;
        const Surfel& sa = (*surfels)[a.surfel];
        const Surfel& sb = (*surfels)[b.surfel];
        // Value-based total order so ties do not depend on input order.
        if (sa.color.r != sb.color.r) return sa.color.r < sb.color.r;
        if (sa.color.g != sb.color.g) return sa.color.g < sb.color.g;
        if (sa.color.b != sb.color.b) return sa.color.b < sb.color.b;
        for (int i = 0; i < 3; ++i) {
            if (sa.p[i] != sb.p[i]) return sa.p[i] < sb.p[i];
        }
        for (int i = 0; i < 3; ++i) {
            if (sa.a[i] != sb.a[i]) return sa.a[i] < sb.a[i];
        }
        for (int i = 0; i < 3; ++i) {
            if (sa.b[i] != sb.b[i]) return sa.b[i] < sb.b[i];
        }
        if (a.u != b.u) return a.u < b.u;
        return a.v < b.v;
    }
};

// Intersect a view-space ray (parameter = view depth) with one surfel disk.
auto IntersectDisk(const RayDir& ray, const ViewSurfel& s, double near_d, double far_d,
                   double* depth, double* u, double* v, double* q) -> bool {
    if (!ray.ok) return false;
    const double denom = Dot(ray.d, s.n);
    const double scale = Len(ray.d);  // |n| == 1
    if (std::abs(denom) <= 1e-12 * scale) return false;  // near-parallel: no spurious hit
    const double t = Dot(s.p, s.n) / denom;
    if (!(t >= near_d) || !(t <= far_d)) return false;

    const D3 h = Mul(ray.d, t);
    const D3 w = Sub(h, s.p);
    // Tangential solve: u = dot(w, b x n)/|a x b|, v = dot(n x a, w)/|a x b|.
    const double ab = std::sqrt(s.nn);
    if (!(ab > 1e-30)) return false;
    const D3 bxn = Cross(s.b, s.n);
    const D3 nxa = Cross(s.n, s.a);
    const double uu = Dot(w, bxn) / ab;
    const double vv = Dot(nxa, w) / ab;
    const double qq = uu * uu + vv * vv;
    if (!(qq < 1.0)) return false;
    if (!std::isfinite(qq)) return false;

    *depth = t;
    *u = uu;
    *v = vv;
    *q = qq;
    return true;
}

} // namespace

/* ------------------------------------------------------------------------- */
/* Surfel helper                                                             */
/* ------------------------------------------------------------------------- */

namespace {

// Relative collinearity: reject |a x b| <= 1e-12 * |a| * |b| (and any zero
// vector).  This is scale-invariant, unlike an absolute threshold.
auto NormalizedCross(const std::array<double, 3>& a, const std::array<double, 3>& b,
                     std::array<double, 3>& out) -> bool {
    if (!Finite(a[0]) || !Finite(a[1]) || !Finite(a[2]) ||
        !Finite(b[0]) || !Finite(b[1]) || !Finite(b[2]))
        return false;
    const D3 cr = Cross(a, b);
    const double la = Len(a), lb = Len(b);
    const double l = Len(cr);
    if (!Finite(l) || !(la > 0.0) || !(lb > 0.0)) return false;
    if (!(l > 1e-12 * la * lb)) return false;
    out = Mul(cr, 1.0 / l);
    return true;
}

} // namespace

auto Surfel::NormalFromAxes(const std::array<double, 3>& a,
                            const std::array<double, 3>& b,
                            std::array<double, 3>& out) -> bool {
    return NormalizedCross(a, b, out);
}

auto Surfel::UnitNormal(std::array<double, 3>& out) const -> bool {
    return NormalizedCross(a, b, out);
}

/* ------------------------------------------------------------------------- */
/* Validation                                                                */
/* ------------------------------------------------------------------------- */

auto ValidateCamera(const SurfelCamera& camera, int width, int height) -> bool {
    if (width <= 0 || height <= 0) return false;
    if (static_cast<long long>(width) * height > (1LL << 40)) return false;
    if (!Finite(camera.near_depth) || !Finite(camera.far_depth)) return false;
    if (!(camera.near_depth > 0.0) || !(camera.far_depth > camera.near_depth)) return false;
    if (!FiniteMatrix(camera.view) || !FiniteMatrix(camera.projection)) return false;
    if (!IsRigid(camera.view)) return false;
    if (!Finite(camera.light_direction)) return false;
    if (!(Len(camera.light_direction) > 1e-12)) return false;

    const Mat4d inv_proj = camera.projection.Inverse();
    const Mat4d inv_view = camera.view.Inverse();
    if (!FiniteMatrix(inv_proj) || !FiniteMatrix(inv_view)) return false;

    // Conventional perspective: clip.w = -view.z.
    if (std::abs(camera.projection(3, 0)) > 1e-12 ||
        std::abs(camera.projection(3, 1)) > 1e-12 ||
        std::abs(camera.projection(3, 3)) > 1e-12)
        return false;
    if (std::abs(camera.projection(3, 2) + 1.0) > 1e-9) return false;

    auto NdcZ = [&](double z) {
        const double w = camera.projection(3, 2) * z + camera.projection(3, 3);
        if (std::abs(w) < 1e-300) return std::numeric_limits<double>::quiet_NaN();
        return (camera.projection(2, 2) * z + camera.projection(2, 3)) / w;
    };
    const double nz_n = NdcZ(-camera.near_depth);
    const double nz_f = NdcZ(-camera.far_depth);
    if (!Finite(nz_n) || !Finite(nz_f)) return false;
    if (std::abs(nz_n + 1.0) > 1e-7 || std::abs(nz_f - 1.0) > 1e-7) return false;

    // A pixel ray must point into the scene for every pixel.
    const double nx0 = 2.0 * 0.5 / width - 1.0;
    const double ny0 = 2.0 * 0.5 / height - 1.0;
    const auto v = inv_proj.Times(D3{nx0, ny0, 0.0}, 1.0);
    if (!(std::abs(v[3]) > 1e-300)) return false;
    if (!(v[2] / v[3] < 0.0)) return false;
    return true;
}

/* ------------------------------------------------------------------------- */
/* Render                                                                    */
/* ------------------------------------------------------------------------- */

auto RenderSurfels(const std::vector<Surfel>& surfels, const SurfelCamera& camera,
                   const BaseImage& base, const SurfelOptions& options,
                   SurfelResult* out) -> bool {
    if (!out) return false;

    const int W = base.width;
    const int H = base.height;
    if (W <= 0 || H <= 0) return false;
    const size_t n_pixels = static_cast<size_t>(W) * static_cast<size_t>(H);
    if (base.color.size() != n_pixels || base.depth.size() != n_pixels) return false;
    if (!(options.delta >= 0.0) || !Finite(options.delta)) return false;
    if (options.tile_size <= 0 || options.tile_size > 1 << 20) return false;
    if (!ValidateCamera(camera, W, H)) return false;

    for (size_t i = 0; i < n_pixels; ++i) {
        if (!Finite(base.color[i])) return false;
        const double d = base.depth[i];
        if (std::isnan(d) || d == -std::numeric_limits<double>::infinity()) return false;
        if (std::isfinite(d) && !(d > 0.0)) return false;
    }
    for (const auto& s : surfels) {
        if (!Finite(s.p) || !Finite(s.a) || !Finite(s.b)) return false;
        if (!ValidColor(s.color)) return false;
        D3 n;
        if (!Surfel::NormalFromAxes(s.a, s.b, n)) return false;  // collinear axes
    }

    const Mat4d inv_proj = camera.projection.Inverse();
    const double near_d = camera.near_depth, far_d = camera.far_depth;
    const int ts = options.tile_size;
    const int tile_cols = (W + ts - 1) / ts;
    const int tile_rows = (H + ts - 1) / ts;

    // --- Prepare view-space sursels and support bounds --------------------
    std::vector<ViewSurfel> view(surfels.size());
    std::vector<SupportBounds> bounds(surfels.size());
    std::vector<SurfelPerPoint> per_point(surfels.size());

    for (size_t i = 0; i < surfels.size(); ++i) {
        const Surfel& s = surfels[i];
        ViewSurfel& v = view[i];
        const auto pv = camera.view.Times(s.p, 1.0);
        v.p = {pv[0], pv[1], pv[2]};
        v.a = camera.view.TimesVector(s.a);
        v.b = camera.view.TimesVector(s.b);
        const D3 cr = Cross(v.a, v.b);
        v.nn = Dot(cr, cr);
        v.n = Normalize(cr);
        // The near/far "slab" in view space is z in [-far, -near].  A disk is
        // fully inside when neither tangent has a z component that can push it
        // across a plane; here we use a tight centre+radius interval.
        const double rad = std::sqrt(v.a[2] * v.a[2] + v.b[2] * v.b[2]);
        const double center_depth = -v.p[2];
        v.depth_min = center_depth - rad;
        v.depth_max = center_depth + rad;

        ComputeSupport(camera.projection, W, H, near_d, far_d, v, &bounds[i]);
        per_point[i].support_kind = bounds[i].kind;
        if (bounds[i].valid) {
            per_point[i].support_x_min = bounds[i].x_min;
            per_point[i].support_x_max = bounds[i].x_max;
            per_point[i].support_y_min = bounds[i].y_min;
            per_point[i].support_y_max = bounds[i].y_max;
        }
    }

    // --- Tile references ---------------------------------------------------
    std::vector<std::vector<int>> tile_refs(static_cast<size_t>(tile_cols) * tile_rows);
    for (size_t i = 0; i < surfels.size(); ++i) {
        if (!bounds[i].valid) continue;
        // Exact continuous support -> integer pixel rectangle.  A pixel centre
        // lies at x+0.5, so floor(x_min) .. ceil(x_max) is a conservative
        // superset of the touched pixels and stays exact in the infimum
        // sense; the tiling rect widens it by one more pixel for rounding.
        int x0 = static_cast<int>(std::floor(bounds[i].x_min));
        int x1 = static_cast<int>(std::ceil(bounds[i].x_max));
        int y0 = static_cast<int>(std::floor(bounds[i].y_min));
        int y1 = static_cast<int>(std::ceil(bounds[i].y_max));
        per_point[i].screen_bounds = PixelRect{
            std::max(x0, 0), std::max(y0, 0),
            std::min(x1, W - 1), std::min(y1, H - 1)};
        int tx0 = std::max(x0 - 1, 0), ty0 = std::max(y0 - 1, 0);
        int tx1 = std::min(x1 + 1, W - 1), ty1 = std::min(y1 + 1, H - 1);
        if (tx1 < tx0 || ty1 < ty0) continue;
        per_point[i].tile_bounds = PixelRect{tx0, ty0, tx1, ty1};
        const int cx0 = tx0 / ts, cx1 = tx1 / ts;
        const int cy0 = ty0 / ts, cy1 = ty1 / ts;
        for (int ty = cy0; ty <= cy1; ++ty)
            for (int tx = cx0; tx <= cx1; ++tx) {
                tile_refs[static_cast<size_t>(ty) * tile_cols + tx]
                    .push_back(static_cast<int>(i));
                per_point[i].tile_refs++;
            }
    }

    // --- Render ------------------------------------------------------------
    SurfelResult result;
    result.width = W;
    result.height = H;
    result.color = base.color;
    result.depth = base.depth;

    std::vector<Hit> hits;
    hits.reserve(16);
    const Vec light_vec({static_cast<float>(camera.light_direction[0]),
                         static_cast<float>(camera.light_direction[1]),
                         static_cast<float>(camera.light_direction[2]), 0});

    size_t total_tests = 0, total_accepted = 0;
    size_t pixels_with_hits = 0, pixels_modified = 0;

    for (int ty = 0; ty < tile_rows; ++ty) {
        for (int tx = 0; tx < tile_cols; ++tx) {
            const auto& refs = tile_refs[static_cast<size_t>(ty) * tile_cols + tx];
            if (refs.empty()) continue;
            const int px0 = tx * ts, py0 = ty * ts;
            const int px1 = std::min(W, px0 + ts), py1 = std::min(H, py0 + ts);

            for (int y = py0; y < py1; ++y) {
                for (int x = px0; x < px1; ++x) {
                    const size_t pi = static_cast<size_t>(y) * W + x;
                    const double base_d = result.depth[pi];
                    if (!(base_d > near_d)) {
                        // Nothing can be strictly in front of a base depth at
                        // or before the near plane.
                        continue;
                    }

                    const RayDir ray = MakeRay(inv_proj, W, H, x, y);
                    if (!ray.ok) continue;

                    hits.clear();
                    for (int idx : refs) {
                        per_point[idx].intersection_tests++;
                        total_tests++;
                        double d, u, v, q;
                        if (!IntersectDisk(ray, view[idx], near_d, far_d, &d, &u, &v, &q)) continue;
                        if (!(d < base_d)) continue;  // strictly in front of the base
                        hits.push_back(Hit{d, u, v, q, idx});
                        per_point[idx].accepted_hits++;
                        total_accepted++;
                    }
                    if (hits.empty()) continue;

                    // Nearest view depth, then the delta band around it.
                    double d0 = std::numeric_limits<double>::infinity();
                    for (const auto& h : hits) d0 = std::min(d0, h.depth);
                    const double cutoff = d0 + options.delta;

                    // Keep the hits with depth <= cutoff, in a value-based
                    // order independent of the input ordering.
                    size_t n = 0;
                    for (size_t j = 0; j < hits.size(); ++j)
                        if (hits[j].depth <= cutoff) hits[n++] = hits[j];
                    hits.resize(n);
                    std::sort(hits.begin(), hits.end(), HitKeyLess{&surfels});

                    double sum_w = 0.0;
                    double acc_r = 0.0, acc_g = 0.0, acc_b = 0.0;
                    for (const auto& h : hits) {
                        const double w = (1.0 - h.q) * (1.0 - h.q);
                        if (!(w > 0.0)) continue;
                        const ViewSurfel& vs = view[h.surfel];
                        const D3 hp = Mul(ray.d, h.depth);
                        const D3 vdir = Normalize(Mul(hp, -1.0));
                        D3 nrm = vs.n;
                        if (Dot(nrm, vdir) < 0.0) nrm = Mul(nrm, -1.0);
                        const Vec fs = FragmentShader(
                            Vec({static_cast<float>(nrm[0]), static_cast<float>(nrm[1]),
                                 static_cast<float>(nrm[2]), 0}),
                            light_vec,
                            Vec({static_cast<float>(vdir[0]), static_cast<float>(vdir[1]),
                                 static_cast<float>(vdir[2]), 0}));
                        const Color& sc = surfels[h.surfel].color;
                        acc_r += w * sc.r * fs[0];
                        acc_g += w * sc.g * fs[1];
                        acc_b += w * sc.b * fs[2];
                        sum_w += w;
                    }
                    if (!(sum_w > 0.0)) continue;

                    const Color blended(static_cast<float>(acc_r / sum_w),
                                        static_cast<float>(acc_g / sum_w),
                                        static_cast<float>(acc_b / sum_w));
                    result.color[pi] = blended;
                    result.depth[pi] = d0;
                    ++pixels_with_hits;
                    if (blended != base.color[pi]) ++pixels_modified;
                }
            }
        }
    }

    // --- Public statistics -------------------------------------------------
    SurfelStats stats;
    stats.width = W;
    stats.height = H;
    stats.tile_cols = tile_cols;
    stats.tile_rows = tile_rows;
    stats.tile_size = ts;
    stats.intersection_tests = total_tests;
    stats.accepted_hits = total_accepted;
    stats.pixels_with_hits = pixels_with_hits;
    stats.pixels_modified = pixels_modified;
    stats.tile_refs = std::move(tile_refs);
    stats.per_point = std::move(per_point);

    std::vector<bool> seen(surfels.size(), false);
    for (const auto& row : stats.tile_refs)
        for (int idx : row) seen[static_cast<size_t>(idx)] = true;
    for (size_t i = 0; i < seen.size(); ++i)
        if (seen[i]) stats.candidates.push_back(static_cast<int>(i));

    result.stats = std::move(stats);
    *out = std::move(result);
    return true;
}

auto RenderSurfels(const std::vector<Surfel>& surfels, const SurfelCamera& camera,
                   const BaseImage& base, const SurfelOptions& options)
    -> std::optional<SurfelResult> {
    SurfelResult result;
    if (!RenderSurfels(surfels, camera, base, options, &result)) return std::nullopt;
    return result;
}

auto BaseFromTarget(const Target& target) -> BaseImage {
    BaseImage base;
    base.width = target.width();
    base.height = target.height();
    const size_t n = target.size();
    base.color.assign(target.color(), target.color() + n);
    base.depth.assign(target.depth(), target.depth() + n);
    return base;
}

/* ------------------------------------------------------------------------- */
/* PPM                                                                        */
/* ------------------------------------------------------------------------- */

namespace {

auto ClampByte(float x) -> uint8_t {
    const double v = std::clamp(static_cast<double>(x), 0.0, 1.0);
    return static_cast<uint8_t>(std::lround(v * 255.0));
}

auto NextToken(const uint8_t* bytes, size_t size, size_t* pos, std::string* tok) -> bool {
    while (*pos < size) {
        const uint8_t c = bytes[*pos];
        if (c == '#') {
            while (*pos < size && bytes[*pos] != '\n') ++*pos;
        } else if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v') {
            ++*pos;
        } else {
            break;
        }
    }
    if (*pos >= size) return false;
    const size_t start = *pos;
    while (*pos < size) {
        const uint8_t c = bytes[*pos];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v' || c == '#')
            break;
        ++*pos;
    }
    tok->assign(reinterpret_cast<const char*>(bytes + start), *pos - start);
    return !tok->empty();
}

} // namespace

auto DecodePPM(const uint8_t* bytes, size_t size, BaseImage* out) -> bool {
    if (!bytes || !out) return false;
    size_t pos = 0;
    std::string magic, sw, sh, maxv;
    if (!NextToken(bytes, size, &pos, &magic) || magic != "P6") return false;
    if (!NextToken(bytes, size, &pos, &sw)) return false;
    if (!NextToken(bytes, size, &pos, &sh)) return false;
    if (!NextToken(bytes, size, &pos, &maxv)) return false;
    int w, h, maxval;
    try {
        w = std::stoi(sw);
        h = std::stoi(sh);
        maxval = std::stoi(maxv);
    } catch (...) {
        return false;
    }
    if (w <= 0 || h <= 0 || maxval <= 0 || maxval > 65535) return false;
    // Exactly one whitespace byte separates the header from the raster.
    if (pos >= size) return false;
    const uint8_t sep = bytes[pos];
    if (!(sep == ' ' || sep == '\t' || sep == '\r' || sep == '\n' || sep == '\f' || sep == '\v'))
        return false;
    ++pos;
    if (sep == '\r' && pos < size && bytes[pos] == '\n') ++pos;

    const size_t need = static_cast<size_t>(w) * h * 3;
    if (size - pos < need) return false;

    BaseImage img;
    img.width = w;
    img.height = h;
    img.color.resize(static_cast<size_t>(w) * h);
    img.depth.assign(static_cast<size_t>(w) * h, std::numeric_limits<double>::infinity());
    const double inv = 1.0 / maxval;
    const uint8_t* src = bytes + pos;
    for (int y = 0; y < h; ++y) {
        const int src_row = h - 1 - y;  // stored top-down -> y-up
        for (int x = 0; x < w; ++x) {
            const uint8_t* p = src + (static_cast<size_t>(src_row) * w + x) * 3;
            img.color[static_cast<size_t>(y) * w + x] =
                Color(static_cast<float>(p[0] * inv), static_cast<float>(p[1] * inv),
                      static_cast<float>(p[2] * inv));
        }
    }
    *out = std::move(img);
    return true;
}

auto EncodePPM(const Color* color, int width, int height, std::vector<uint8_t>* out) -> bool {
    if (!color || !out || width <= 0 || height <= 0) return false;
    out->clear();
    const std::string header =
        "P6\n" + std::to_string(width) + " " + std::to_string(height) + "\n255\n";
    out->insert(out->end(), header.begin(), header.end());
    const size_t offset = out->size();
    out->resize(offset + static_cast<size_t>(width) * height * 3);
    uint8_t* p = out->data() + offset;
    for (int y = height - 1; y >= 0; --y) {  // y-up array -> top-down file
        for (int x = 0; x < width; ++x) {
            const Color& c = color[static_cast<size_t>(y) * width + x];
            *p++ = ClampByte(c.r);
            *p++ = ClampByte(c.g);
            *p++ = ClampByte(c.b);
        }
    }
    return true;
}

} // namespace gfx
