#pragma once

#include <valarray>
#include <cmath>

namespace gfx {

/* Need to be careful with this: you should never use auto when referring
 * to a std::valarray. "You get an object of the expression template type,
 * which holds a dangling reference to an expired temporary"
 * https://gcc.gnu.org/bugzilla/show_bug.cgi?id=83860
 */
typedef std::valarray<float> Vec;
auto dot(const Vec& a, const Vec& b) -> float;
auto normalize(const Vec& v) -> Vec;
auto cross(const Vec& a, const Vec& b) -> Vec;
auto xy(const Vec& v) -> Vec;
auto xyz(const Vec& v) -> Vec;

// Explicit component views.  std::valarray's "never use auto" rule makes
// v[i] awkward to use in generic code, so the surface code uses these.
inline auto xyz_of(const Vec& v) -> Vec { return {v[0], v[1], v[2], 0}; }
inline auto length(const Vec& v) -> float { return std::sqrt(dot(v, v)); }
// Rodrigues' rotation: right-handed rotation of v around unit axis k.
inline auto rotate_axis(const Vec& v, const Vec& k, float angle) -> Vec {
    auto c = std::cos(angle), s = std::sin(angle);
    return c * v + (s * dot(k, v)) * k + s * cross(k, v);
}

struct Mat4 {
    constexpr static size_t N = 4;
    Vec values;

    Mat4() : values(N * N) {}
    Mat4(const Vec& v) : values(v) {}
    auto& operator()(size_t i, size_t j) { return values[i * N + j]; }
    const auto& operator()(size_t i, size_t j) const { return values[i * N + j]; }

    auto operator*(const Vec& v) const -> Vec;
    auto operator*(const Mat4& m) const -> Mat4;
    auto Transpose() const -> Mat4;
};

struct Quaternion {
    float w, x, y, z;

    static auto FromEuler(float x, float y, float z) -> Quaternion;

    Quaternion(float w, float x, float y, float z) : w(w), x(x), y(y), z(z) {}

    auto operator*(const Quaternion& q) const -> Quaternion;
    auto AsMatrix() -> Mat4;
};

}
