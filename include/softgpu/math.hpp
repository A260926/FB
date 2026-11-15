#pragma once
#include <cmath>
#include <cstdint>

namespace softgpu {

constexpr float kPi = 3.14159265358979f;

struct Vec2 {
  float x = 0, y = 0;
};

struct Vec3 {
  float x = 0, y = 0, z = 0;
  Vec3 operator+(Vec3 o) const { return {x + o.x, y + o.y, z + o.z}; }
  Vec3 operator-(Vec3 o) const { return {x - o.x, y - o.y, z - o.z}; }
  Vec3 operator*(float s) const { return {x * s, y * s, z * s}; }
  Vec3 operator*(Vec3 o) const { return {x * o.x, y * o.y, z * o.z}; }
  Vec3 operator-() const { return {-x, -y, -z}; }
};

struct Vec4 {
  float x = 0, y = 0, z = 0, w = 0;
  Vec4() = default;
  Vec4(float x_, float y_, float z_, float w_) : x(x_), y(y_), z(z_), w(w_) {}
  Vec4(Vec3 v, float w_) : x(v.x), y(v.y), z(v.z), w(w_) {}
  Vec3 xyz() const { return {x, y, z}; }
  Vec4 operator+(Vec4 o) const { return {x + o.x, y + o.y, z + o.z, w + o.w}; }
  Vec4 operator-(Vec4 o) const { return {x - o.x, y - o.y, z - o.z, w - o.w}; }
  Vec4 operator*(float s) const { return {x * s, y * s, z * s, w * s}; }
};

inline float dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline Vec3 cross(Vec3 a, Vec3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
inline float length(Vec3 v) { return std::sqrt(dot(v, v)); }
inline Vec3 normalize(Vec3 v) {
  const float l = length(v);
  return l > 0 ? v * (1.0f / l) : v;
}
inline float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
inline float lerp(float a, float b, float t) { return a + (b - a) * t; }

// Column-major 4x4, m[col][row], so m * v applies the columns as basis vectors (OpenGL layout).
struct Mat4 {
  float m[4][4] = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}, {0, 0, 0, 1}};

  static Mat4 identity() { return Mat4{}; }

  Vec4 operator*(Vec4 v) const {
    return {m[0][0] * v.x + m[1][0] * v.y + m[2][0] * v.z + m[3][0] * v.w,
            m[0][1] * v.x + m[1][1] * v.y + m[2][1] * v.z + m[3][1] * v.w,
            m[0][2] * v.x + m[1][2] * v.y + m[2][2] * v.z + m[3][2] * v.w,
            m[0][3] * v.x + m[1][3] * v.y + m[2][3] * v.z + m[3][3] * v.w};
  }
  Mat4 operator*(const Mat4& b) const {
    Mat4 r;
    for (int c = 0; c < 4; ++c)
      for (int rr = 0; rr < 4; ++rr)
        r.m[c][rr] = m[0][rr] * b.m[c][0] + m[1][rr] * b.m[c][1] + m[2][rr] * b.m[c][2] + m[3][rr] * b.m[c][3];
    return r;
  }
  // Upper-left 3x3 only: for normals under rotation/uniform scale.
  Vec3 transform_dir(Vec3 v) const {
    return {m[0][0] * v.x + m[1][0] * v.y + m[2][0] * v.z, m[0][1] * v.x + m[1][1] * v.y + m[2][1] * v.z,
            m[0][2] * v.x + m[1][2] * v.y + m[2][2] * v.z};
  }
  Vec3 transform_point(Vec3 p) const {
    const Vec4 r = *this * Vec4(p, 1.0f);
    return {r.x / r.w, r.y / r.w, r.z / r.w};
  }

  static Mat4 translate(Vec3 t) {
    Mat4 r;
    r.m[3][0] = t.x;
    r.m[3][1] = t.y;
    r.m[3][2] = t.z;
    return r;
  }
  static Mat4 scale(Vec3 s) {
    Mat4 r;
    r.m[0][0] = s.x;
    r.m[1][1] = s.y;
    r.m[2][2] = s.z;
    return r;
  }
  static Mat4 rotate_x(float a) {
    Mat4 r;
    const float c = std::cos(a), s = std::sin(a);
    r.m[1][1] = c;
    r.m[1][2] = s;
    r.m[2][1] = -s;
    r.m[2][2] = c;
    return r;
  }
  static Mat4 rotate_y(float a) {
    Mat4 r;
    const float c = std::cos(a), s = std::sin(a);
    r.m[0][0] = c;
    r.m[0][2] = -s;
    r.m[2][0] = s;
    r.m[2][2] = c;
    return r;
  }
  static Mat4 rotate_z(float a) {
    Mat4 r;
    const float c = std::cos(a), s = std::sin(a);
    r.m[0][0] = c;
    r.m[0][1] = s;
    r.m[1][0] = -s;
    r.m[1][1] = c;
    return r;
  }
  // Right-handed, camera looks down -z, clip z in [-w, w] (OpenGL convention).
  static Mat4 perspective(float fovy, float aspect, float znear, float zfar) {
    Mat4 r;
    const float f = 1.0f / std::tan(fovy * 0.5f);
    r.m[0][0] = f / aspect;
    r.m[1][1] = f;
    r.m[2][2] = (zfar + znear) / (znear - zfar);
    r.m[2][3] = -1.0f;
    r.m[3][2] = 2.0f * zfar * znear / (znear - zfar);
    r.m[3][3] = 0.0f;
    return r;
  }
  static Mat4 orthographic(float l, float r_, float b, float t, float n, float f) {
    Mat4 r;
    r.m[0][0] = 2.0f / (r_ - l);
    r.m[1][1] = 2.0f / (t - b);
    r.m[2][2] = -2.0f / (f - n);
    r.m[3][0] = -(r_ + l) / (r_ - l);
    r.m[3][1] = -(t + b) / (t - b);
    r.m[3][2] = -(f + n) / (f - n);
    return r;
  }
  static Mat4 look_at(Vec3 eye, Vec3 at, Vec3 up) {
    const Vec3 f = normalize(at - eye);
    const Vec3 s = normalize(cross(f, up));
    const Vec3 u = cross(s, f);
    Mat4 r;
    r.m[0][0] = s.x;
    r.m[1][0] = s.y;
    r.m[2][0] = s.z;
    r.m[0][1] = u.x;
    r.m[1][1] = u.y;
    r.m[2][1] = u.z;
    r.m[0][2] = -f.x;
    r.m[1][2] = -f.y;
    r.m[2][2] = -f.z;
    r.m[3][0] = -dot(s, eye);
    r.m[3][1] = -dot(u, eye);
    r.m[3][2] = dot(f, eye);
    return r;
  }
};

}  // namespace softgpu
