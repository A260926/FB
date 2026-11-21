#pragma once

/* Shared interface between the triangle rasterizer and the CPU surfel path.
 *
 * FragmentShader() is the single shading implementation used by both paths:
 * it is a free function in namespace gfx (see rasterizer.cpp), so there is
 * exactly one Phong implementation and no per-example copy.
 */

#include <array>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <type_traits>
#include <vector>

#include "math.h"

namespace gfx {

template <typename T> class MatView;
using Mat4d = MatView<double>;
using Mat4f = MatView<float>;

// Linear RGB colour.  Channels may temporarily live outside [0,1] only inside
// the surfel reconstruction; all public entry points validate their inputs.
struct Color {
    float r = 0, g = 0, b = 0;

    constexpr Color() = default;
    constexpr Color(float r, float g, float b) : r(r), g(g), b(b) {}

    constexpr auto operator+(const Color& o) const -> Color { return {r + o.r, g + o.g, b + o.b}; }
    constexpr auto operator*(float s) const -> Color { return {r * s, g * s, b * s}; }
    constexpr auto operator*(const Color& o) const -> Color { return {r * o.r, g * o.g, b * o.b}; }
    constexpr auto operator==(const Color& o) const -> bool {
        return r == o.r && g == o.g && b == o.b;
    }
    constexpr auto operator!=(const Color& o) const -> bool { return !(*this == o); }
    auto& operator+=(const Color& o) { r += o.r; g += o.g; b += o.b; return *this; }

    static constexpr auto FromVec(const Vec& v) -> Color { return {v[0], v[1], v[2]}; }
    auto ToVec() const -> Vec { return {r, g, b, 0}; }
};

// The Phong shader, shared by triangles and surfels.  n is the shading normal
// (already flipped towards the eye), l the direction towards the light (view
// space), v the direction from the surface point towards the eye (view space).
auto FragmentShader(Vec n, Vec l, Vec v) -> Vec;

// Opaque render target: linear RGB plus view-space depth (positive distance,
// +infinity where nothing was written).  y is up; the array index is
// y * width + x, matching the surfel path.
class Target {
public:
    Target() = default;
    Target(int width, int height)
        : width_(width), height_(height),
          color_(static_cast<size_t>(width) * height, Color{}),
          depth_(static_cast<size_t>(width) * height, kEmptyDepth) {}

    void Reset() {
        std::fill(color_.begin(), color_.end(), Color{});
        std::fill(depth_.begin(), depth_.end(), kEmptyDepth);
    }

    auto width() const -> int { return width_; }
    auto height() const -> int { return height_; }
    auto size() const -> size_t { return color_.size(); }
    auto empty() const -> bool { return color_.empty(); }

    auto color() -> Color* { return color_.data(); }
    auto color() const -> const Color* { return color_.data(); }
    auto depth() -> double* { return depth_.data(); }
    auto depth() const -> const double* { return depth_.data(); }

    auto At(int x, int y) -> Color& { return color_[static_cast<size_t>(y) * width_ + x]; }
    auto At(int x, int y) const -> const Color& { return color_[static_cast<size_t>(y) * width_ + x]; }
    auto DepthAt(int x, int y) const -> double { return depth_[static_cast<size_t>(y) * width_ + x]; }

    // Shared shading helper: shades one fragment with the common shader and
    // writes it if it is strictly in front of the current depth.
    auto ShadeAndWrite(int x, int y, float view_depth, const Vec& n, const Vec& l, const Vec& v) -> bool {
        if (!(view_depth < depth_[static_cast<size_t>(y) * width_ + x])) return false;
        Vec c = FragmentShader(n, l, v);
        color_[static_cast<size_t>(y) * width_ + x] = Color::FromVec(c);
        depth_[static_cast<size_t>(y) * width_ + x] = view_depth;
        return true;
    }

    static constexpr double kEmptyDepth = std::numeric_limits<double>::infinity();

private:
    int width_ = 0, height_ = 0;
    std::vector<Color> color_;
    std::vector<double> depth_;
};

// Triangle rasterizer writing into a Target.  Uses the same FragmentShader as
// the surfel path.  Implemented in rasterizer.cpp; the legacy Rasterize()
// entry point (raw ARGB + clip-z buffers) is kept for the original window and
// its headless baseline.  All matrices are double-precision row-major views so
// the shared surfel camera can feed it directly.
auto RasterizeTriangle(const Mat4d& projection_mat, const Mat4d& model_view_mat,
                       const Mat4d& normal_mat,
                       const Vec& v0, const Vec& v1, const Vec& v2,
                       const Vec& n0, const Vec& n1, const Vec& n2,
                       Target* target) -> void;

/* ------------------------------------------------------------------------- */
/* Typed matrix view over the legacy gfx::Mat4 (row-major).                   */
/* ------------------------------------------------------------------------- */

template <typename T>
class MatView {
public:
    using value_type = T;

    MatView() { for (int i = 0; i < 16; ++i) v_[i] = T(0); }
    MatView(std::initializer_list<T> init) {
        int i = 0;
        for (auto x : init) { if (i < 16) v_[i++] = x; }
        while (i < 16) v_[i++] = T(0);
    }
    // Accepts gfx::Mat4 (or anything convertible to Vec) so callers can keep
    // handing us the original matrices.
    explicit MatView(const Mat4& m) {
        for (int i = 0; i < 16; ++i) v_[i] = static_cast<T>(m.values[i]);
    }

    auto operator()(int i, int j) -> T& { return v_[i * 4 + j]; }
    auto operator()(int i, int j) const -> T { return v_[i * 4 + j]; }

    static auto Identity() -> MatView {
        MatView m;
        for (int i = 0; i < 4; ++i) m(i, i) = T(1);
        return m;
    }

    auto operator*(const MatView& m) const -> MatView {
        MatView r;
        for (int i = 0; i < 4; ++i)
            for (int j = 0; j < 4; ++j) {
                T s = T(0);
                for (int k = 0; k < 4; ++k) s += v_[i * 4 + k] * m.v_[k * 4 + j];
                r.v_[i * 4 + j] = s;
            }
        return r;
    }

    // Transforms a point (w = 1) or a vector (w = 0), keeping the homogeneous w.
    auto Times(const std::array<T, 3>& p, T w) const -> std::array<T, 4> {
        std::array<T, 4> in{ p[0], p[1], p[2], w };
        std::array<T, 4> out{};
        for (int i = 0; i < 4; ++i)
            for (int k = 0; k < 4; ++k) out[i] += v_[i * 4 + k] * in[k];
        return out;
    }

    // Linear part only (w = 0), dropping the homogeneous component.
    auto TimesVector(const std::array<T, 3>& p) const -> std::array<T, 3> {
        auto r = Times(p, T(0));
        return { r[0], r[1], r[2] };
    }

    auto Transpose() const -> MatView {
        MatView r;
        for (int i = 0; i < 4; ++i)
            for (int j = 0; j < 4; ++j) r.v_[i * 4 + j] = v_[j * 4 + i];
        return r;
    }

    auto Inverse() const -> MatView {
        // Gauss-Jordan on the 4x4 augmented system.
        T a[4][8];
        for (int i = 0; i < 4; ++i)
            for (int j = 0; j < 4; ++j) a[i][j] = v_[i * 4 + j];
        for (int i = 0; i < 4; ++i)
            for (int j = 0; j < 4; ++j) a[i][4 + j] = (i == j) ? T(1) : T(0);
        for (int col = 0; col < 4; ++col) {
            int piv = col;
            for (int r = col + 1; r < 4; ++r)
                if (std::fabs(a[r][col]) > std::fabs(a[piv][col])) piv = r;
            if (a[piv][col] == T(0)) return MatView();
            if (piv != col)
                for (int j = 0; j < 8; ++j) std::swap(a[col][j], a[piv][j]);
            T inv = T(1) / a[col][col];
            for (int j = 0; j < 8; ++j) a[col][j] *= inv;
            for (int r = 0; r < 4; ++r) {
                if (r == col) continue;
                T f = a[r][col];
                if (f == T(0)) continue;
                for (int j = 0; j < 8; ++j) a[r][j] -= f * a[col][j];
            }
        }
        MatView r;
        for (int i = 0; i < 4; ++i)
            for (int j = 0; j < 4; ++j) r.v_[i * 4 + j] = a[i][4 + j];
        return r;
    }

private:
    T v_[16];
};

} // namespace gfx
