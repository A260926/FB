#include "math.h"

namespace gfx {

auto dot(const Vec& a, const Vec& b) -> float { return (a * b).sum(); }
auto normalize(const Vec& v) -> Vec { return v / sqrt((v * v).sum()); }
auto xy(const Vec& v) -> Vec  { return {v[0], v[1], 0, 0}; }
auto xyz(const Vec& v) -> Vec { return {v[0], v[1], v[2], 0}; }
auto cross(const Vec& a, const Vec& b) -> Vec {
    return Vec({a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0], 0});
}

auto Mat4::operator*(const Vec& v) const -> Vec {
    Vec r(N);
    for (size_t i = 0; i < N; ++i)
        r[i] = dot(values[std::slice(i * N, N, 1)], v);
    return r;
}

auto Mat4::operator*(const Mat4& m) const -> Mat4 {
    Mat4 r;
    for (size_t i = 0; i < N; ++i)
        r.values[std::slice(i, N, N)] = *this * m.values[std::slice(i, N, N)];
    return r;
}

auto Mat4::Transpose() const -> Mat4 {
    Mat4 r;
    for (size_t i = 0; i < N; ++i)
        r.values[std::slice(i * N, N, 1)] = values[std::slice(i, N, N)];
    return r;
}

auto Quaternion::FromEuler(float x, float y, float z) -> Quaternion {
    auto cy = cosf(z * 0.5), sy = sinf(z * 0.5), cp = cosf(y * 0.5);
    auto sp = sinf(y * 0.5), cr = cosf(x * 0.5), sr = sinf(x * 0.5);

    return {cy * cp * cr + sy * sp * sr, cy * cp * sr - sy * sp * cr,
            sy * cp * sr + cy * sp * cr, sy * cp * cr - cy * sp * sr};
}

auto Quaternion::operator*(const Quaternion& q) const -> Quaternion {
    return {
        w * q.w - x * q.x - y * q.y - z * q.z,
        w * q.x + x * q.w + y * q.z - z * q.y,
        w * q.y - x * q.z + y * q.w + z * q.x,
        w * q.z + x * q.y - y * q.x + z * q.w
    };
}

auto Quaternion::AsMatrix() -> Mat4 {
    return Mat4({
        1 - 2 * y * y - 2 * z * z, 2 * x * y - 2 * z * w, 2 * x * z + 2 * y * w, 0,
        2 * x * y + 2 * z * w, 1 - 2 * x * x - 2 * z * z, 2 * y * z - 2 * x * w, 0,
        2 * x * z - 2 * y * w, 2 * y * z + 2 * x * w, 1 - 2 * x * x - 2 * y * y, 0,
        0, 0, 0, 1
    });
}

}
