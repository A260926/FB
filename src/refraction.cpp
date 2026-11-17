#include "refraction.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <fstream>
#include <limits>
#include <stdexcept>

namespace gfx::refr {

namespace {

constexpr double kInf = std::numeric_limits<double>::infinity();

auto Finite(const Vec3& v) -> bool {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

// Screen-space barycentric coordinates of a sample point.
auto Barycentric2D(float ax, float ay, float bx, float by, float cx, float cy,
                   float px, float py) -> std::array<float, 3> {
    const float v0x = bx - ax, v0y = by - ay;
    const float v1x = cx - ax, v1y = cy - ay;
    const float v2x = px - ax, v2y = py - ay;
    const float d00 = v0x * v0x + v0y * v0y;
    const float d01 = v0x * v1x + v0y * v1y;
    const float d11 = v1x * v1x + v1y * v1y;
    const float d20 = v2x * v0x + v2y * v0y;
    const float d21 = v2x * v1x + v2y * v1y;
    const float denom = d00 * d11 - d01 * d01;
    if (denom == 0.f) return {-1.f, -1.f, -1.f};
    const float v = (d11 * d20 - d01 * d21) / denom;
    const float w = (d00 * d21 - d01 * d20) / denom;
    return {1.f - v - w, v, w};
}

// Maps a view-space ray o + t*d through the caller's projection matrix and
// solves NDC plane crossings analytically.  With clip.w == -view.z the
// projected ray is a rational linear function of t; every crossing therefore
// comes from the continuous ray parameter rather than fixed stepping.
class RayProjection {
public:
    RayProjection(const Mat4& projection, const Vec3& origin, const Vec3& direction,
                  int width, int height)
        : width_(width), height_(height) {
        for (int row = 0; row < 3; ++row) {
            a_[row] = projection(row, 0) * direction.x + projection(row, 1) * direction.y
                    + projection(row, 2) * direction.z;
            b_[row] = projection(row, 0) * origin.x + projection(row, 1) * origin.y
                    + projection(row, 2) * origin.z + projection(row, 3);
        }
        w0_ = -origin.z;
        wd_ = -direction.z;
    }

    // ndc[row] at parameter t, valid while clip.w(t) > 0.
    auto Ndc(int row, double t) const -> double {
        return (a_[row] * t + b_[row]) / (w0_ + wd_ * t);
    }

    // Parameter t where ndc[row] == s.  Returns infinity when parallel.
    auto Solve(int row, double s) const -> double {
        const double denom = a_[row] - s * wd_;
        const double scale = 1.0 + std::abs(a_[row]) + std::abs(wd_);
        if (std::abs(denom) <= 1e-15 * scale) return kInf;
        return (s * w0_ - b_[row]) / denom;
    }

    // Half-line parameter interval inside the finite frustum
    // (ndc x/y in [-1, 1], ndc z in [0, 1], clip w > 0).  False when empty.
    auto FrustumInterval(double* t_lo, double* t_hi) const -> bool {
        double lo = 0.0, hi = kInf;
        if (std::abs(wd_) > 1e-15 * (1.0 + std::abs(w0_))) {
            const double tw = -w0_ / wd_;
            if (wd_ > 0.0) lo = std::max(lo, tw); else hi = std::min(hi, tw);
        } else if (!(w0_ > 0.0)) {
            return false;
        }
        static constexpr std::array<std::array<double, 2>, 3> kBounds = {{
            {{-1.0, 1.0}}, {{-1.0, 1.0}}, {{0.0, 1.0}}
        }};
        for (int row = 0; row < 3; ++row) {
            for (int side = 0; side < 2; ++side) {
                const double s = kBounds[row][side];
                // w > 0, so side 0 is the lower bound (ndc >= s) and side 1
                // is the upper bound (ndc <= s).
                const double A = a_[row] - s * wd_;
                const double B = b_[row] - s * w0_;
                const double scale = 1.0 + std::abs(a_[row]) + std::abs(wd_);
                if (std::abs(A) <= 1e-15 * scale) {
                    if (side == 0 ? B < 0.0 : B > 0.0) return false;
                    continue;
                }
                const double t = -B / A;
                if (side == 0) {
                    if (A > 0.0) lo = std::max(lo, t); else hi = std::min(hi, t);
                } else {
                    if (A > 0.0) hi = std::min(hi, t); else lo = std::max(lo, t);
                }
            }
        }
        if (!(hi > lo)) return false;
        *t_lo = lo;
        *t_hi = hi;
        return true;
    }

    // Cell containing the ray at parameter t; false outside the grid.
    auto CellAt(double t, int* cx, int* cy) const -> bool {
        const double px = (Ndc(0, t) + 1.0) * 0.5 * width_;
        const double py = (Ndc(1, t) + 1.0) * 0.5 * height_;
        if (!std::isfinite(px) || !std::isfinite(py)) return false;
        *cx = static_cast<int>(std::floor(px));
        *cy = static_cast<int>(std::floor(py));
        return *cx >= 0 && *cx < width_ && *cy >= 0 && *cy < height_;
    }

    // Parameter of the next vertical target-grid line strictly beyond t.
    auto NextXBoundary(int cx, double t) const -> double {
        const double slope = a_[0] * w0_ - b_[0] * wd_; // sign of d(ndc_x)/dt
        if (std::abs(slope) <= 1e-18 * (1.0 + std::abs(a_[0]) * std::abs(w0_))) return kInf;
        const int k = slope > 0.0 ? cx + 1 : cx;
        if (k <= 0 || k >= width_) return kInf;
        const double root = Solve(0, 2.0 * k / width_ - 1.0);
        return root > t ? root : kInf;
    }

    auto NextYBoundary(int cy, double t) const -> double {
        const double slope = a_[1] * w0_ - b_[1] * wd_;
        if (std::abs(slope) <= 1e-18 * (1.0 + std::abs(a_[1]) * std::abs(w0_))) return kInf;
        const int k = slope > 0.0 ? cy + 1 : cy;
        if (k <= 0 || k >= height_) return kInf;
        const double root = Solve(1, 2.0 * k / height_ - 1.0);
        return root > t ? root : kInf;
    }

private:
    int width_;
    int height_;
    double a_[3] = {0, 0, 0};
    double b_[3] = {0, 0, 0};
    double w0_ = 0.;
    double wd_ = 0.;
};

struct ProjectedVertex {
    Vec3 view_position;
    Vec3 view_normal;
    float inv_w = 0.f;
    float ndc_z = 0.f;
    float screen_x = 0.f;
    float screen_y = 0.f;
};

auto PrepareVertex(const Mat4& view, const Mat4& projection, const Mat4& normal_matrix,
                   const Vec& world_pos, const Vec& world_normal,
                   int width, int height) -> ProjectedVertex {
    Vec view4 = view * world_pos;
    Vec clip = projection * view4;
    const float w = clip[3];
    if (!(w > 0.f) || !std::isfinite(w))
        throw std::invalid_argument("refraction: vertex has non-positive clip w");

    Vec n4 = normal_matrix * world_normal;
    ProjectedVertex v;
    v.view_position = Vec3(view4[0], view4[1], view4[2]);
    v.view_normal = Vec3(n4[0], n4[1], n4[2]);
    v.inv_w = 1.f / w;
    v.ndc_z = clip[2] / w;
    v.screen_x = (clip[0] / w + 1.f) * 0.5f * width;
    v.screen_y = (clip[1] / w + 1.f) * 0.5f * height;
    if (!std::isfinite(v.ndc_z) || !std::isfinite(v.screen_x) || !std::isfinite(v.screen_y)
        || !Finite(v.view_position) || !Finite(v.view_normal))
        throw std::invalid_argument("refraction: non-finite transformed vertex");
    return v;
}

// Shared fill-triangle production path, factored out of the old
// Rasterize/Render geometry walk.  The writer receives every covered pixel
// centre with its 1/w perspective-correct interpolants.
template <typename Writer>
void FillTriangle(const Mat4& view, const Mat4& projection, const Mat4& normal_matrix,
                  const Triangle& triangle, int width, int height, FaceMode mode,
                  Writer&& write) {
    ProjectedVertex v[3];
    for (int i = 0; i < 3; ++i)
        v[i] = PrepareVertex(view, projection, normal_matrix, triangle.position[i],
                             triangle.normal[i], width, height);

    // Same screen-space winding sign as the original rasterizer; the FaceMode
    // selects which side survives (the back shell needs inverted culling).
    const float edge = (v[1].screen_x - v[0].screen_x) * (v[2].screen_y - v[0].screen_y)
                     - (v[1].screen_y - v[0].screen_y) * (v[2].screen_x - v[0].screen_x);
    if (edge == 0.f) return;
    if (mode == FaceMode::FrontFaces && edge < 0.f) return;
    if (mode == FaceMode::BackFaces && edge > 0.f) return;

    const float min_x = std::min({v[0].screen_x, v[1].screen_x, v[2].screen_x});
    const float max_x = std::max({v[0].screen_x, v[1].screen_x, v[2].screen_x});
    const float min_y = std::min({v[0].screen_y, v[1].screen_y, v[2].screen_y});
    const float max_y = std::max({v[0].screen_y, v[1].screen_y, v[2].screen_y});

    const int x0 = std::max(0, static_cast<int>(std::floor(min_x)));
    const int y0 = std::max(0, static_cast<int>(std::floor(min_y)));
    const int x1 = std::min(width, static_cast<int>(std::floor(max_x)) + 1);
    const int y1 = std::min(height, static_cast<int>(std::floor(max_y)) + 1);

    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) {
            // Pixel (x, y) centre is (x + 0.5, y + 0.5).
            auto [u, q, r] = Barycentric2D(
                v[0].screen_x, v[0].screen_y, v[1].screen_x, v[1].screen_y,
                v[2].screen_x, v[2].screen_y, x + 0.5f, y + 0.5f);
            // A sub-pixel edge tolerance keeps a sample sitting on a shared
            // diagonal from being dropped by floating-point sign noise.
            constexpr float kEdgeTol = 1e-5f;
            if (u < -kEdgeTol || q < -kEdgeTol || r < -kEdgeTol) continue;
            u = std::max(u, 0.f);
            q = std::max(q, 0.f);
            r = std::max(r, 0.f);
            const float weight_sum = u + q + r;
            if (!(weight_sum > 0.f)) continue;
            u /= weight_sum;
            q /= weight_sum;
            r /= weight_sum;

            // 1/w perspective interpolation for every interpolant.
            const float inv_denom = u * v[0].inv_w + q * v[1].inv_w + r * v[2].inv_w;
            if (!(inv_denom > 0.f)) continue;
            const float recip = 1.f / inv_denom;
            Vec3 position = (v[0].view_position * (u * v[0].inv_w)
                           + v[1].view_position * (q * v[1].inv_w)
                           + v[2].view_position * (r * v[2].inv_w)) * recip;
            Vec3 normal = (v[0].view_normal * (u * v[0].inv_w)
                         + v[1].view_normal * (q * v[1].inv_w)
                         + v[2].view_normal * (r * v[2].inv_w)) * recip;
            // Only the normal is re-normalized after interpolation.
            normal = Normalize(normal);
            const float eye_depth = -position.z;
            if (!std::isfinite(eye_depth) || eye_depth <= 0.f) continue;
            write(x, y, eye_depth, position, normal);
        }
    }
}

// Depth-first grid walk shared by the glass back field and the background.
// depth_at(x, y) returns the positive eye-space plane depth of the cell, or a
// non-positive value when the cell has no record.
template <typename Accessor>
auto WalkGrid(const RayProjection& ray, Accessor&& depth_at, int width, int height,
              const Vec3& origin, const Vec3& direction) {
    struct WalkOutcome {
        bool hit = false;
        int cell_x = -1;
        int cell_y = -1;
        double ndc_x = 0.;
        double ndc_y = 0.;
        double hit_t = 0.;
        size_t cells_visited = 0;
    } outcome;

    double t_lo = 0., t_hi = kInf;
    if (!ray.FrustumInterval(&t_lo, &t_hi)) return outcome;

    int cx = 0, cy = 0;
    if (!ray.CellAt(t_lo, &cx, &cy)) return outcome;

    const double eps = 1e-10 * std::max(1.0, t_hi);
    double t = t_lo;
    while (true) {
        ++outcome.cells_visited;
        if (outcome.cells_visited > static_cast<size_t>(width + height + 1)) {
            outcome.cells_visited = width + height + 1;
            break;
        }

        const float cell_depth = depth_at(cx, cy);
        double t_next = std::min(ray.NextXBoundary(cx, t), ray.NextYBoundary(cy, t));

        if (cell_depth > 0.f && std::isfinite(cell_depth)) {
            // Continuous intersection of the ray with the cell's depth plane
            // -view.z == cell_depth.  Self-intersections at t <= 0 are ignored.
            double t_hit = kInf;
            if (std::abs(direction.z) > 1e-15f)
                t_hit = (-origin.z - static_cast<double>(cell_depth)) / direction.z;
            const bool inside = t_next == kInf ? t_hit <= t_hi + eps
                                               : t_hit <= t_next + eps;
            if (t_hit > 0.0 && t_hit > eps && t_hit <= t_hi + eps && inside) {
                outcome.hit = true;
                outcome.cell_x = cx;
                outcome.cell_y = cy;
                outcome.hit_t = t_hit;
                outcome.ndc_x = ray.Ndc(0, t_hit);
                outcome.ndc_y = ray.Ndc(1, t_hit);
                return outcome;
            }
        }

        if (t_next == kInf || t_next >= t_hi - eps) break;
        t = t_next;
        int nx = cx, ny = cy;
        const double nudge = 1e-12 * std::max(1.0, std::abs(t));
        if (!ray.CellAt(t + nudge, &nx, &ny)) break;
        if (nx == cx && ny == cy) break;
        cx = nx;
        cy = ny;
    }
    return outcome;
}

} // namespace

auto Dot(const Vec3& a, const Vec3& b) -> float {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

auto Cross(const Vec3& a, const Vec3& b) -> Vec3 {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

auto Length(const Vec3& a) -> float { return std::sqrt(Dot(a, a)); }

auto Normalize(const Vec3& a) -> Vec3 {
    const float len = Length(a);
    if (!(len > 0.f)) throw std::invalid_argument("refraction: zero-length vector");
    return a * (1.f / len);
}

auto ToVec4(const Vec3& v, float w) -> Vec { return Vec({v.x, v.y, v.z, w}); }

auto IdentityMat4() -> Mat4 {
    Mat4 m;
    for (int i = 0; i < 4; ++i) m(i, i) = 1.f;
    return m;
}

auto MakePerspective(float l, float r, float b, float t, float n, float f) -> Mat4 {
    if (!(n > 0.f) || !(f > n) || !(r > l) || !(t > b))
        throw std::invalid_argument("refraction: invalid perspective frustum");
    return Mat4(Vec{
        2.f * n / (r - l), 0.f, (r + l) / (r - l), 0.f,
        0.f, 2.f * n / (t - b), (t + b) / (t - b), 0.f,
        0.f, 0.f, -f / (f - n), -f * n / (f - n),
        0.f, 0.f, -1.f, 0.f
    });
}

auto RefractSnell(const Vec3& incident, const Vec3& outward_normal, float eta,
                  Vec3* refracted, float* cos_incident) -> bool {
    Vec3 n = outward_normal;
    float cosi = -Dot(incident, n);
    if (cosi < 0.f) {
        // Incidence from the outward side of the stored normal.
        n = n * -1.f;
        cosi = -cosi;
    }
    *cos_incident = cosi;
    const float k = 1.f - eta * eta * (1.f - cosi * cosi);
    if (k < 0.f) return false; // total internal reflection
    *refracted = incident * eta + n * (eta * cosi - std::sqrt(k));
    *refracted = Normalize(*refracted);
    return true;
}

auto FresnelUnpolarized(float cos_incident, float eta) -> float {
    float cosi = cos_incident;
    if (cosi < 0.f) cosi = -cosi;
    cosi = std::clamp(cosi, 0.f, 1.f);
    const float sin2_t = eta * eta * (1.f - cosi * cosi);
    if (sin2_t >= 1.f) return 1.f; // total internal reflection
    const float cost = std::sqrt(1.f - sin2_t);
    const float rs = (cosi - (1.f / eta) * cost) / (cosi + (1.f / eta) * cost);
    const float rp = (eta * cost - cosi) / (eta * cost + cosi);
    return 0.5f * (rs * rs + rp * rp);
}

// ---------------------------------------------------------------------------
// DepthTarget
// ---------------------------------------------------------------------------

DepthTarget::DepthTarget(int width, int height)
    : width_(width), height_(height),
      cells_(static_cast<size_t>(width) * height) {
    if (width <= 0 || height <= 0)
        throw std::invalid_argument("refraction: target dimensions must be positive");
}

void DepthTarget::RasterizeTriangle(const Mat4& view, const Mat4& projection,
                                    const Mat4& normal_matrix,
                                    const Triangle& triangle, FaceMode mode) {
    FillTriangle(view, projection, normal_matrix, triangle, width_, height_, mode,
                 [&](int x, int y, float eye_depth, const Vec3& position, const Vec3& normal) {
                     CellRecord& cell = cells_[y * width_ + x];
                     // Original depth rule: nearest fragment wins, ties overwrite.
                     if (cell.valid && eye_depth > cell.view_depth) return;
                     cell.valid = true;
                     cell.view_depth = eye_depth;
                     cell.position = position;
                     cell.normal = normal;
                 });
}

auto DepthTarget::IsComplete() const -> bool {
    for (const CellRecord& cell : cells_) {
        if (!cell.valid) return false;
        if (!std::isfinite(cell.view_depth) || cell.view_depth <= 0.f || !Finite(cell.position)
            || !Finite(cell.normal))
            return false;
    }
    return true;
}

void DepthTarget::Clear() { std::fill(cells_.begin(), cells_.end(), CellRecord{}); }

// ---------------------------------------------------------------------------
// BackgroundTarget
// ---------------------------------------------------------------------------

BackgroundTarget::BackgroundTarget(int width, int height)
    : width_(width), height_(height),
      colors_(static_cast<size_t>(width) * height),
      depths_(static_cast<size_t>(width) * height,
              std::numeric_limits<float>::infinity()),
      valid_(static_cast<size_t>(width) * height, 0) {
    if (width <= 0 || height <= 0)
        throw std::invalid_argument("refraction: target dimensions must be positive");
}

void BackgroundTarget::RasterizeTriangle(const Mat4& view, const Mat4& projection,
                                         const Triangle& triangle,
                                         const Vec3& linear_color) {
    if (!Finite(linear_color) || linear_color.x < 0.f || linear_color.y < 0.f
        || linear_color.z < 0.f)
        throw std::invalid_argument("refraction: background color must be finite non-negative");
    // The background never uses normals; supply a finite identity normal
    // matrix so the shared fill path's interpolants stay well defined.
    FillTriangle(view, projection, IdentityMat4(), triangle, width_, height_,
                 FaceMode::FrontFaces,
                 [&](int x, int y, float eye_depth, const Vec3&, const Vec3&) {
                     const size_t idx = static_cast<size_t>(y) * width_ + x;
                     if (valid_[idx] && eye_depth > depths_[idx]) return;
                     valid_[idx] = 1;
                     depths_[idx] = eye_depth;
                     colors_[idx] = linear_color;
                 });
}

auto BackgroundTarget::IsComplete() const -> bool {
    for (size_t i = 0; i < valid_.size(); ++i) {
        if (!valid_[i]) return false;
        if (!std::isfinite(depths_[i]) || depths_[i] <= 0.f || !Finite(colors_[i]))
            return false;
    }
    return true;
}

void BackgroundTarget::Clear() {
    std::fill(colors_.begin(), colors_.end(), Vec3{});
    std::fill(depths_.begin(), depths_.end(), std::numeric_limits<float>::infinity());
    std::fill(valid_.begin(), valid_.end(), 0);
}

// ---------------------------------------------------------------------------
// Field walks
// ---------------------------------------------------------------------------

auto TraceDepthField(const DepthTarget& field, const Mat4& projection,
                     const Vec3& origin, const Vec3& direction) -> ExitTrace {
    ExitTrace result;
    if (!Finite(origin) || !Finite(direction) || Length(direction) == 0.f)
        throw std::invalid_argument("refraction: trace ray must be finite and non-degenerate");
    const Vec3 d = Normalize(direction);
    RayProjection ray(projection, origin, d, field.width(), field.height());

    auto depth_at = [&](int x, int y) -> float {
        const CellRecord& cell = field.cell(x, y);
        return cell.valid ? cell.view_depth : -1.f;
    };
    auto walk = WalkGrid(ray, depth_at, field.width(), field.height(), origin, d);
    result.cells_visited = walk.cells_visited;
    result.entry_distance = Length(origin);
    if (!walk.hit) {
        result.status = TraceStatus::ExitMissing;
        return result;
    }

    const CellRecord& cell = field.cell(walk.cell_x, walk.cell_y);
    result.hit = true;
    result.status = TraceStatus::ExitFound;
    result.cell_x = walk.cell_x;
    result.cell_y = walk.cell_y;
    result.ndc_x = walk.ndc_x;
    result.ndc_y = walk.ndc_y;
    result.exit_t = walk.hit_t;
    result.exit_position = origin + d * static_cast<float>(walk.hit_t);
    result.exit_normal = cell.normal;
    result.internal_distance = walk.hit_t; // d is unit length
    return result;
}

auto TraceBackground(const BackgroundTarget& background, const Mat4& projection,
                     const Vec3& origin, const Vec3& direction) -> BackgroundTrace {
    BackgroundTrace result;
    if (!Finite(origin) || !Finite(direction) || Length(direction) == 0.f)
        throw std::invalid_argument("refraction: trace ray must be finite and non-degenerate");
    const Vec3 d = Normalize(direction);
    RayProjection ray(projection, origin, d, background.width(), background.height());

    auto depth_at = [&](int x, int y) -> float {
        return background.valid(x, y) ? background.depth(x, y) : -1.f;
    };
    auto walk = WalkGrid(ray, depth_at, background.width(), background.height(), origin, d);
    result.cells_visited = walk.cells_visited;
    if (!walk.hit) return result;

    result.hit = true;
    result.cell_x = walk.cell_x;
    result.cell_y = walk.cell_y;
    result.ndc_x = walk.ndc_x;
    result.ndc_y = walk.ndc_y;
    result.distance = walk.hit_t;
    result.color = background.color(walk.cell_x, walk.cell_y);
    return result;
}

// ---------------------------------------------------------------------------
// Synthesis
// ---------------------------------------------------------------------------

namespace {

void ValidateCaptures(const DepthTarget& front, const DepthTarget& back,
                      const BackgroundTarget& background) {
    if (front.width() != back.width() || front.width() != background.width()
        || front.height() != back.height() || front.height() != background.height())
        throw std::invalid_argument("refraction: capture targets must share dimensions");

    auto check_field = [](const DepthTarget& field) {
        for (size_t i = 0; i < field.size(); ++i) {
            const CellRecord& c = field.cell(static_cast<int>(i % field.width()),
                                             static_cast<int>(i / field.width()));
            if (!c.valid) continue;
            if (!std::isfinite(c.view_depth) || c.view_depth <= 0.f || !Finite(c.position)
                || !Finite(c.normal))
                throw std::invalid_argument("refraction: non-finite glass capture record");
            if (std::abs(Length(c.normal) - 1.f) > 1e-4f)
                throw std::invalid_argument("refraction: glass normal is not unit length");
        }
    };
    check_field(front);
    check_field(back);
    for (int y = 0; y < background.height(); ++y) {
        for (int x = 0; x < background.width(); ++x) {
            if (!background.valid(x, y)) continue;
            const Vec3& c = background.color(x, y);
            if (!std::isfinite(background.depth(x, y)) || background.depth(x, y) <= 0.f
                || !Finite(c) || c.x < 0.f || c.y < 0.f || c.z < 0.f)
                throw std::invalid_argument("refraction: invalid background record");
        }
    }
}

} // namespace

auto Synthesize(const DepthTarget& front, const DepthTarget& back,
                const BackgroundTarget& background, const Mat4& projection,
                const GlassMaterial& material, const Vec3& ambient) -> SynthesisResult {
    if (!std::isfinite(material.ior) || material.ior < 1.01f || material.ior > 2.5f)
        throw std::invalid_argument("refraction: IOR must be finite in [1.01, 2.5]");
    if (!Finite(material.sigma_a) || material.sigma_a.x < 0.f || material.sigma_a.y < 0.f
        || material.sigma_a.z < 0.f)
        throw std::invalid_argument("refraction: sigmaA must be finite non-negative");
    if (!Finite(ambient) || ambient.x < 0.f || ambient.y < 0.f || ambient.z < 0.f)
        throw std::invalid_argument("refraction: ambient must be finite non-negative");
    for (int i = 0; i < 16; ++i)
        if (!std::isfinite(projection.values[i]))
            throw std::invalid_argument("refraction: projection matrix must be finite");
    ValidateCaptures(front, back, background);

    const int width = front.width();
    const int height = front.height();
    SynthesisResult result;
    result.color.resize(static_cast<size_t>(width) * height);

    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const size_t idx = static_cast<size_t>(y) * width + x;
            const CellRecord& entry = front.cell(x, y);
            const bool bg_valid = background.valid(x, y);

            // No glass here: the opaque background shows through directly.
            if (!entry.valid) {
                result.color[idx] = bg_valid ? background.color(x, y) : ambient;
                if (bg_valid) ++result.stats.background_hits;
                continue;
            }
            ++result.stats.entry_pixels;

            const Vec3 entry_position = entry.position;
            const Vec3 entry_normal = entry.normal;
            const float entry_depth = entry.view_depth;

            // An entry hidden behind the opaque background keeps the background.
            if (bg_valid && background.depth(x, y) <= entry_depth) {
                result.color[idx] = background.color(x, y);
                ++result.stats.occluded_entries;
                ++result.stats.background_hits;
                continue;
            }

            const Vec3 camera_ray = Normalize(entry_position); // camera at view origin
            float cos_entry = 0.f;
            Vec3 internal_ray;
            const float f_entry = FresnelUnpolarized(-Dot(camera_ray, entry_normal),
                                                      1.f / material.ior);
            const bool refracted_in = RefractSnell(camera_ray, entry_normal,
                                                   1.f / material.ior,
                                                   &internal_ray, &cos_entry);

            Vec3 color = ambient * f_entry; // Fresnel reflection of the environment

            if (refracted_in) {
                ExitTrace exit_trace = TraceDepthField(back, projection,
                                                       entry_position, internal_ray);
                result.stats.max_cells_per_segment =
                    std::max(result.stats.max_cells_per_segment, exit_trace.cells_visited);

                bool transmitted = false;
                Vec3 out_ray;
                float internal_length = 0.f;
                float f_exit = 0.f;

                if (exit_trace.status == TraceStatus::ExitMissing) {
                    ++result.stats.exit_missing;
                } else {
                    internal_length = static_cast<float>(exit_trace.internal_distance);
                    float cos_exit = 0.f;
                    const bool refracted_out = RefractSnell(
                        internal_ray, exit_trace.exit_normal, material.ior,
                        &out_ray, &cos_exit);
                    f_exit = FresnelUnpolarized(cos_exit, material.ior);
                    if (!refracted_out || f_exit >= 1.f) {
                        ++result.stats.total_internal_reflection;
                    } else {
                        transmitted = true;
                        ++result.stats.exit_hits;
                    }
                }

                if (transmitted) {
                    BackgroundTrace bg_trace = TraceBackground(background, projection,
                                                               exit_trace.exit_position,
                                                               out_ray);
                    result.stats.max_cells_per_segment =
                        std::max(result.stats.max_cells_per_segment,
                                 bg_trace.cells_visited);
                    const Vec3 behind = bg_trace.hit ? bg_trace.color : ambient;
                    if (bg_trace.hit) ++result.stats.background_hits;
                    else ++result.stats.background_misses;

                    const float t_entry = 1.f - f_entry;
                    const float t_exit = 1.f - f_exit;
                    const Vec3 attenuation{
                        std::exp(-material.sigma_a.x * internal_length),
                        std::exp(-material.sigma_a.y * internal_length),
                        std::exp(-material.sigma_a.z * internal_length)};
                    color += behind * attenuation * (t_entry * t_exit);
                }
            } else {
                // Air->glass with IOR > 1 cannot TIR; keep reflection only.
                ++result.stats.total_internal_reflection;
            }

            result.color[idx] = color;
        }
    }
    return result;
}

void WritePPM(const std::string& path, const std::vector<Vec3>& linear,
              int width, int height) {
    if (width <= 0 || height <= 0
        || linear.size() != static_cast<size_t>(width) * height)
        throw std::invalid_argument("refraction: PPM buffer does not match dimensions");
    std::ofstream file(path, std::ios::binary);
    if (!file) throw std::runtime_error("refraction: cannot open PPM output: " + path);
    file << "P6\n" << width << ' ' << height << "\n255\n";
    auto quantize = [](float v) -> unsigned char {
        if (!(v >= 0.f)) v = 0.f;
        if (v > 1.f) v = 1.f;
        return static_cast<unsigned char>(std::lround(v * 255.f));
    };
    std::vector<unsigned char> row(static_cast<size_t>(width) * 3);
    for (int r = 0; r < height; ++r) {
        const int y = height - 1 - r; // storage is y-up, PPM rows are top-down
        for (int x = 0; x < width; ++x) {
            const Vec3& c = linear[static_cast<size_t>(y) * width + x];
            row[x * 3 + 0] = quantize(c.x);
            row[x * 3 + 1] = quantize(c.y);
            row[x * 3 + 2] = quantize(c.z);
        }
        file.write(reinterpret_cast<const char*>(row.data()),
                   static_cast<std::streamsize>(row.size()));
    }
    if (!file) throw std::runtime_error("refraction: failed writing PPM: " + path);
}

} // namespace gfx::refr
