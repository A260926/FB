// Dual quaternion skinning target tests.
//
// This file deliberately uses its own scalar double-precision quaternion,
// matrix and dual-quaternion formulas.  It does not call the implementation's
// internal math helpers, so numerical results are checked against an
// independent calculation rather than only checking radius or finiteness.

#include <ozz/base/maths/simd_math.h>
#include <ozz/base/span.h>
#include <ozz/geometry/runtime/skinning_job.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <vector>

namespace {

int g_failures = 0;

void Check(bool _condition, const char* _message) {
  if (!_condition) {
    std::printf("FAIL: %s\n", _message);
    ++g_failures;
  }
}

void Near(double _actual, double _expected, double _epsilon,
          const char* _message) {
  if (std::fabs(_actual - _expected) > _epsilon) {
    std::printf("FAIL: %s: actual=%.9g expected=%.9g delta=%.3g\n", _message,
                _actual, _expected, std::fabs(_actual - _expected));
    ++g_failures;
  }
}

struct V3 {
  double x = 0.0, y = 0.0, z = 0.0;
};

V3 operator+(const V3& a, const V3& b) {
  return {a.x + b.x, a.y + b.y, a.z + b.z};
}
V3 operator-(const V3& a, const V3& b) {
  return {a.x - b.x, a.y - b.y, a.z - b.z};
}
V3 operator*(const V3& a, double s) { return {a.x * s, a.y * s, a.z * s}; }
double Dot(const V3& a, const V3& b) {
  return a.x * b.x + a.y * b.y + a.z * b.z;
}
V3 Cross(const V3& a, const V3& b) {
  return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z,
          a.x * b.y - a.y * b.x};
}
double Length(const V3& v) { return std::sqrt(Dot(v, v)); }
V3 Normalize(const V3& v) { return v * (1.0 / Length(v)); }

struct Q {
  double x = 0.0, y = 0.0, z = 0.0, w = 1.0;
};

Q operator+(const Q& a, const Q& b) {
  return {a.x + b.x, a.y + b.y, a.z + b.z, a.w + b.w};
}
Q operator*(const Q& q, double s) {
  return {q.x * s, q.y * s, q.z * s, q.w * s};
}
Q operator-(const Q& q) { return {-q.x, -q.y, -q.z, -q.w}; }
double Dot(const Q& a, const Q& b) {
  return a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
}

Q AxisAngle(const V3& axis, double angle) {
  const V3 n = Normalize(axis);
  const double h = angle * 0.5;
  const double s = std::sin(h);
  return {n.x * s, n.y * s, n.z * s, std::cos(h)};
}

V3 Rotate(const Q& q, const V3& v) {
  const V3 u{q.x, q.y, q.z};
  const V3 t = Cross(u, v) * 2.0;
  return v + t * q.w + Cross(u, t);
}

struct M4 {
  double m[4][4] = {};
};

M4 Identity() {
  M4 r;
  for (int i = 0; i < 4; ++i) r.m[i][i] = 1.0;
  return r;
}

V3 TransformPoint(const M4& m, const V3& v) {
  return {m.m[0][0] * v.x + m.m[0][1] * v.y + m.m[0][2] * v.z + m.m[0][3],
          m.m[1][0] * v.x + m.m[1][1] * v.y + m.m[1][2] * v.z + m.m[1][3],
          m.m[2][0] * v.x + m.m[2][1] * v.y + m.m[2][2] * v.z + m.m[2][3]};
}

V3 TransformVector(const M4& m, const V3& v) {
  return {m.m[0][0] * v.x + m.m[0][1] * v.y + m.m[0][2] * v.z,
          m.m[1][0] * v.x + m.m[1][1] * v.y + m.m[1][2] * v.z,
          m.m[2][0] * v.x + m.m[2][1] * v.y + m.m[2][2] * v.z};
}

M4 Translation(const V3& t) {
  M4 r = Identity();
  r.m[0][3] = t.x;
  r.m[1][3] = t.y;
  r.m[2][3] = t.z;
  return r;
}

// Rotation, then column-wise scale, then translation.
M4 Transform(const Q& q, const V3& t, const V3& scale = {1.0, 1.0, 1.0}) {
  M4 r = Identity();
  const double x2 = q.x + q.x, y2 = q.y + q.y, z2 = q.z + q.z;
  const double xx = q.x * x2, xy = q.x * y2, xz = q.x * z2;
  const double yy = q.y * y2, yz = q.y * z2, zz = q.z * z2;
  const double wx = q.w * x2, wy = q.w * y2, wz = q.w * z2;

  r.m[0][0] = (1.0 - (yy + zz)) * scale.x;
  r.m[0][1] = (xy - wz) * scale.y;
  r.m[0][2] = (xz + wy) * scale.z;
  r.m[1][0] = (xy + wz) * scale.x;
  r.m[1][1] = (1.0 - (xx + zz)) * scale.y;
  r.m[1][2] = (yz - wx) * scale.z;
  r.m[2][0] = (xz - wy) * scale.x;
  r.m[2][1] = (yz + wx) * scale.y;
  r.m[2][2] = (1.0 - (xx + yy)) * scale.z;
  r.m[0][3] = t.x;
  r.m[1][3] = t.y;
  r.m[2][3] = t.z;
  return r;
}

M4 Multiply(const M4& a, const M4& b) {
  M4 r;
  for (int i = 0; i < 4; ++i) {
    for (int j = 0; j < 4; ++j) {
      double sum = 0.0;
      for (int k = 0; k < 4; ++k) sum += a.m[i][k] * b.m[k][j];
      r.m[i][j] = sum;
    }
  }
  return r;
}

M4 Transpose(const M4& a) {
  M4 r;
  for (int i = 0; i < 4; ++i)
    for (int j = 0; j < 4; ++j) r.m[i][j] = a.m[j][i];
  return r;
}

double Cofactor3(const double a[3][3]) {
  return a[0][0] * (a[1][1] * a[2][2] - a[1][2] * a[2][1]) -
         a[0][1] * (a[1][0] * a[2][2] - a[1][2] * a[2][0]) +
         a[0][2] * (a[1][0] * a[2][1] - a[1][1] * a[2][0]);
}

// General 4x4 inverse through cofactors; adequate for test matrices.
M4 Inverse(const M4& a) {
  M4 inv;
  double cof[4][4];
  for (int i = 0; i < 4; ++i) {
    for (int j = 0; j < 4; ++j) {
      double minor[3][3];
      int mi = 0;
      for (int r = 0; r < 4; ++r) {
        if (r == i) continue;
        int mj = 0;
        for (int c = 0; c < 4; ++c) {
          if (c == j) continue;
          minor[mi][mj++] = a.m[r][c];
        }
        ++mi;
      }
      const int sign = ((i + j) & 1) ? -1 : 1;
      cof[i][j] = sign * Cofactor3(minor);
    }
  }
  double det = 0.0;
  for (int j = 0; j < 4; ++j) det += a.m[0][j] * cof[0][j];
  for (int i = 0; i < 4; ++i)
    for (int j = 0; j < 4; ++j) inv.m[j][i] = cof[i][j] / det;
  return inv;
}

ozz::math::Float4x4 ToOzz(const M4& a) {
  ozz::math::Float4x4 r;
  for (int c = 0; c < 4; ++c) {
    r.cols[c] = ozz::math::simd_float4::Load(
        static_cast<float>(a.m[0][c]), static_cast<float>(a.m[1][c]),
        static_cast<float>(a.m[2][c]), static_cast<float>(a.m[3][c]));
  }
  return r;
}

Q MatrixQuaternion(const M4& m) {
  Q q;
  const double trace = m.m[0][0] + m.m[1][1] + m.m[2][2];
  if (trace > 0.0) {
    const double s = 0.5 / std::sqrt(trace + 1.0);
    q.w = 0.25 / s;
    q.x = (m.m[2][1] - m.m[1][2]) * s;
    q.y = (m.m[0][2] - m.m[2][0]) * s;
    q.z = (m.m[1][0] - m.m[0][1]) * s;
  } else if (m.m[0][0] > m.m[1][1] && m.m[0][0] > m.m[2][2]) {
    const double s = 2.0 * std::sqrt(1.0 + m.m[0][0] - m.m[1][1] - m.m[2][2]);
    q.w = (m.m[2][1] - m.m[1][2]) / s;
    q.x = 0.25 * s;
    q.y = (m.m[0][1] + m.m[1][0]) / s;
    q.z = (m.m[0][2] + m.m[2][0]) / s;
  } else if (m.m[1][1] > m.m[2][2]) {
    const double s = 2.0 * std::sqrt(1.0 + m.m[1][1] - m.m[0][0] - m.m[2][2]);
    q.w = (m.m[0][2] - m.m[2][0]) / s;
    q.x = (m.m[0][1] + m.m[1][0]) / s;
    q.y = 0.25 * s;
    q.z = (m.m[1][2] + m.m[2][1]) / s;
  } else {
    const double s = 2.0 * std::sqrt(1.0 + m.m[2][2] - m.m[0][0] - m.m[1][1]);
    q.w = (m.m[1][0] - m.m[0][1]) / s;
    q.x = (m.m[0][2] + m.m[2][0]) / s;
    q.y = (m.m[1][2] + m.m[2][1]) / s;
    q.z = 0.25 * s;
  }

  // Required fixed sign order: w, z, y, x.
  if (q.w != 0.0) {
    if (q.w < 0.0) q = -q;
  } else if (q.z != 0.0) {
    if (q.z < 0.0) q = -q;
  } else if (q.y != 0.0) {
    if (q.y < 0.0) q = -q;
  } else if (q.x < 0.0) {
    q = -q;
  }
  return q;
}

struct DQ {
  Q real;
  Q dual;
};

DQ MakeDQ(const M4& m, const Q& q) {
  const V3 t{m.m[0][3], m.m[1][3], m.m[2][3]};
  Q d;
  d.w = -0.5 * (t.x * q.x + t.y * q.y + t.z * q.z);
  d.x = 0.5 * (t.x * q.w + t.y * q.z - t.z * q.y);
  d.y = 0.5 * (-t.x * q.z + t.y * q.w + t.z * q.x);
  d.z = 0.5 * (t.x * q.y - t.y * q.x + t.z * q.w);
  return {q, d};
}

V3 DualTranslation(const Q& r, const Q& d) {
  return {2.0 * (r.w * d.x - r.x * d.w + r.y * d.z - r.z * d.y),
          2.0 * (r.w * d.y - r.y * d.w + r.z * d.x - r.x * d.z),
          2.0 * (r.w * d.z - r.z * d.w + r.x * d.y - r.y * d.x)};
}

struct Skinned {
  V3 position, normal, tangent;
};

// Independent dual quaternion blend. explicit_weights has count - 1 entries.
Skinned ExpectedDQ(const std::vector<M4>& palette,
                   const std::vector<uint16_t>& indices,
                   const std::vector<double>& explicit_weights, const V3& p,
                   const V3& n, const V3& t) {
  const int count = static_cast<int>(indices.size());
  std::vector<double> weights(count, 0.0);
  double sum = 0.0;
  for (int i = 0; i < count - 1; ++i) {
    weights[i] = explicit_weights[i];
    sum += weights[i];
  }
  if (count > 0) weights[count - 1] = 1.0 - sum;

  int ref_slot = -1;
  uint16_t ref_index = 0;
  double ref_weight = -1.0;
  for (int i = 0; i < count; ++i) {
    if (!(weights[i] > 0.0)) continue;
    const uint16_t index = indices[i];
    if (weights[i] > ref_weight ||
        (weights[i] == ref_weight && index < ref_index)) {
      ref_weight = weights[i];
      ref_index = index;
      ref_slot = i;
    }
  }

  const Q reference = MatrixQuaternion(palette[ref_index]);
  Q real{0, 0, 0, 0};
  Q dual{0, 0, 0, 0};
  for (int i = 0; i < count; ++i) {
    if (!(weights[i] > 0.0)) continue;
    Q q = MatrixQuaternion(palette[indices[i]]);
    if (Dot(q, reference) < 0.0) q = -q;
    const DQ dq = MakeDQ(palette[indices[i]], q);
    real = real + dq.real * weights[i];
    dual = dual + dq.dual * weights[i];
  }

  const double norm = std::sqrt(Dot(real, real));
  // Tests calling this helper only use rigid blends above the fallback limit.
  if (norm < 1e-6) return {{NAN, NAN, NAN}, {NAN, NAN, NAN}, {NAN, NAN, NAN}};
  real = real * (1.0 / norm);
  dual = dual * (1.0 / norm);
  dual = dual + real * (-Dot(real, dual));

  const V3 tr = DualTranslation(real, dual);
  return {Rotate(real, p) + tr, Rotate(real, n), Rotate(real, t)};
}

void ExpectVec3(const float* actual, const V3& expected, double eps,
                const char* message) {
  Near(actual[0], expected.x, eps, message);
  Near(actual[1], expected.y, eps, message);
  Near(actual[2], expected.z, eps, message);
}

bool Vec3Near(const float* actual, const V3& expected, double eps) {
  return std::fabs(actual[0] - expected.x) <= eps &&
         std::fabs(actual[1] - expected.y) <= eps &&
         std::fabs(actual[2] - expected.z) <= eps;
}

ozz::span<const float> FloatSpan(const std::vector<float>& v) {
  return ozz::span<const float>(v.data(), v.size());
}
ozz::span<float> MutableFloatSpan(std::vector<float>& v) {
  return ozz::span<float>(v.data(), v.size());
}

void TestLargeTwist() {
  constexpr int kVertices = 12;
  constexpr int kInfluences = 2;
  const double pi = std::acos(-1.0);
  const double twist = 100.0 * pi / 180.0;

  std::vector<M4> matrices;
  matrices.push_back(Transform(AxisAngle({0, 0, 1}, twist), {0, 0, 0}));
  matrices.push_back(Transform(AxisAngle({0, 0, 1}, -twist), {0, 0, 0}));
  ozz::math::Float4x4 ozz_matrices[2] = {ToOzz(matrices[0]),
                                         ToOzz(matrices[1])};

  uint16_t indices[kVertices * kInfluences];
  float weights[kVertices];
  std::vector<float> in(kVertices * 3), dq_out(kVertices * 3),
      lbs_out(kVertices * 3);

  for (int i = 0; i < kVertices; ++i) {
    const double a = 2.0 * pi * i / kVertices;
    const double radius = 0.37;
    in[i * 3 + 0] = static_cast<float>(radius * std::cos(a));
    in[i * 3 + 1] = static_cast<float>(radius * std::sin(a));
    in[i * 3 + 2] = 0.21f;
    indices[i * 2 + 0] = 0;
    indices[i * 2 + 1] = 1;
    weights[i] = 0.5f;
    dq_out[i * 3 + 0] = dq_out[i * 3 + 1] = dq_out[i * 3 + 2] = -77.f;
  }

  ozz::geometry::SkinningJob job;
  job.vertex_count = kVertices;
  job.influences_count = kInfluences;
  job.skinning_mode = ozz::geometry::SkinningJob::kDualQuaternion;
  job.joint_matrices = ozz_matrices;
  job.joint_indices = ozz::make_span(indices);
  job.joint_indices_stride = kInfluences * sizeof(uint16_t);
  job.joint_weights = ozz::make_span(weights);
  job.joint_weights_stride = sizeof(float);
  job.in_positions = FloatSpan(in);
  job.in_positions_stride = 3 * sizeof(float);
  job.out_positions = MutableFloatSpan(dq_out);
  job.out_positions_stride = 3 * sizeof(float);
  Check(job.Validate() && job.Run(), "large twist DQ run");

  ozz::geometry::SkinningJob linear = job;
  linear.skinning_mode = ozz::geometry::SkinningJob::kLinear;
  linear.out_positions = MutableFloatSpan(lbs_out);
  Check(linear.Run(), "large twist LBS run");

  for (int i = 0; i < kVertices; ++i) {
    const V3 p{in[i * 3], in[i * 3 + 1], in[i * 3 + 2]};
    const Skinned e = ExpectedDQ(matrices, {0, 1}, {0.5}, p, {}, {});
    ExpectVec3(&dq_out[i * 3], e.position, 1e-4, "twist DQ coordinate");
    const double r = std::hypot(dq_out[i * 3], dq_out[i * 3 + 1]);
    Near(r, 0.37, 1e-4, "twist DQ radius");
    Near(dq_out[i * 3 + 2], 0.21, 1e-4, "twist DQ z");

    const double lbs_radius =
        std::hypot(lbs_out[i * 3], lbs_out[i * 3 + 1]);
    Check(lbs_radius < 0.08, "linear baseline really collapses twist radius");
    Check(r > lbs_radius + 0.25, "DQ preserves substantially more radius");
  }
}

void TestTranslationsAndDifferentAxes() {
  std::vector<M4> palette;
  palette.push_back(Transform(AxisAngle({1, 0, 0}, 0.71), {1.0, 0.2, -0.3}));
  palette.push_back(
      Transform(AxisAngle({1, 2, 3}, 1.37), {-0.4, 0.5, 0.8}));
  palette.push_back(
      Transform(AxisAngle({0, 1, 1}, -0.93), {0.2, -1.0, 0.3}));
  ozz::math::Float4x4 ozz_palette[3] = {
      ToOzz(palette[0]), ToOzz(palette[1]), ToOzz(palette[2])};

  const uint16_t indices[3] = {2, 0, 1};
  float weights[2] = {0.5f, 0.3f};  // last restored to 0.2.
  const float in_p[3] = {0.31f, -0.42f, 0.57f};
  const float in_n[3] = {0.2f, 0.8f, -0.565685f};
  const float in_t[3] = {-0.707107f, 0.707107f, 0.0f};
  float out_p[3], out_n[3], out_t[3];

  ozz::geometry::SkinningJob job;
  job.vertex_count = 1;
  job.influences_count = 3;
  job.skinning_mode = ozz::geometry::SkinningJob::kDualQuaternion;
  job.joint_matrices = ozz_palette;
  job.joint_indices = ozz::make_span(indices);
  job.joint_indices_stride = sizeof(indices);
  job.joint_weights = ozz::make_span(weights);
  job.joint_weights_stride = sizeof(weights);
  job.in_positions = in_p;
  job.in_positions_stride = sizeof(in_p);
  job.out_positions = out_p;
  job.out_positions_stride = sizeof(out_p);
  job.in_normals = in_n;
  job.in_normals_stride = sizeof(in_n);
  job.out_normals = out_n;
  job.out_normals_stride = sizeof(out_n);
  job.in_tangents = in_t;
  job.in_tangents_stride = sizeof(in_t);
  job.out_tangents = out_t;
  job.out_tangents_stride = sizeof(out_t);
  Check(job.Run(), "translation/axis DQ run");

  const V3 p{0.31, -0.42, 0.57};
  const V3 n = Normalize({0.2, 0.8, -0.565685});
  const V3 t = Normalize({-0.707107, 0.707107, 0.0});
  const Skinned e = ExpectedDQ(palette, {2, 0, 1}, {0.5, 0.3}, p, n, t);
  ExpectVec3(out_p, e.position, 1e-4, "translated position");
  ExpectVec3(out_n, e.normal, 1e-4, "rotated normal");
  ExpectVec3(out_t, e.tangent, 1e-4, "rotated tangent");
  Near(Length({out_n[0], out_n[1], out_n[2]}), 1.0, 1e-5,
       "normal length preserved");
  Near(Length({out_t[0], out_t[1], out_t[2]}), 1.0, 1e-5,
       "tangent length preserved");
}

void TestInfluenceCountsAndTie() {
  constexpr int kMax = 6;
  std::vector<M4> palette;
  for (int i = 0; i < 8; ++i) {
    const V3 axis{0.3 + 0.13 * i, 1.0 - 0.11 * i, 0.7 + 0.07 * i};
    const Q q = AxisAngle(axis, 0.23 + 0.31 * i);
    const V3 tr{0.11 * i - 0.2, 0.07 * i, -0.05 * i + 0.1};
    palette.push_back(Transform(q, tr));
  }
  std::vector<ozz::math::Float4x4> ozz_palette;
  for (const M4& m : palette) ozz_palette.push_back(ToOzz(m));

  const int counts[] = {1, 2, 4, 6};
  for (int count : counts) {
    uint16_t indices[kMax] = {3, 0, 5, 2, 7, 1};
    float weights[kMax - 1] = {0.37f, 0.1f, 0.2f, 0.12f, 0.18f};
    // The first values are selected according to count below.
    if (count == 4) {
      weights[0] = 0.1f;
      weights[1] = 0.2f;
      weights[2] = 0.3f;
    } else if (count == 6) {
      weights[0] = 0.12f;
      weights[1] = 0.18f;
      weights[2] = 0.07f;
      weights[3] = 0.23f;
      weights[4] = 0.15f;
    }

    float in_p[3] = {0.2f, -0.3f, 0.4f};
    float in_n[3] = {1, 0, 0};
    float in_t[3] = {0, 1, 0};
    float out_p[3] = {}, out_n[3] = {}, out_t[3] = {};

    ozz::geometry::SkinningJob job;
    job.vertex_count = 1;
    job.influences_count = count;
    job.skinning_mode = ozz::geometry::SkinningJob::kDualQuaternion;
    job.joint_matrices = ozz::span<ozz::math::Float4x4>(
        ozz_palette.data(), ozz_palette.size());
    job.joint_indices = ozz::span<uint16_t>(indices, count);
    job.joint_indices_stride = count * sizeof(uint16_t);
    if (count > 1) {
      job.joint_weights = ozz::span<float>(weights, count - 1);
      job.joint_weights_stride = (count - 1) * sizeof(float);
    }
    job.in_positions = in_p;
    job.in_positions_stride = sizeof(in_p);
    job.out_positions = out_p;
    job.out_positions_stride = sizeof(out_p);
    job.in_normals = in_n;
    job.in_normals_stride = sizeof(in_n);
    job.out_normals = out_n;
    job.out_normals_stride = sizeof(out_n);
    job.in_tangents = in_t;
    job.in_tangents_stride = sizeof(in_t);
    job.out_tangents = out_t;
    job.out_tangents_stride = sizeof(out_t);

    char message[128];
    std::snprintf(message, sizeof(message), "influence count %d run", count);
    Check(job.Run(), message);

    std::vector<uint16_t> expected_indices(indices, indices + count);
    std::vector<double> expected_weights;
    for (int i = 0; i < count - 1; ++i) expected_weights.push_back(weights[i]);
    const Skinned e = ExpectedDQ(palette, expected_indices, expected_weights,
                                 {0.2, -0.3, 0.4}, {1, 0, 0}, {0, 1, 0});
    std::snprintf(message, sizeof(message), "count %d position", count);
    ExpectVec3(out_p, e.position, 1e-4, message);
    std::snprintf(message, sizeof(message), "count %d normal", count);
    ExpectVec3(out_n, e.normal, 1e-4, message);
    std::snprintf(message, sizeof(message), "count %d tangent", count);
    ExpectVec3(out_t, e.tangent, 1e-4, message);

    if (count == 1) {
      const V3 p{0.2, -0.3, 0.4};
      ExpectVec3(out_p, TransformPoint(palette[3], p), 1e-4,
                 "single influence reconstructs matrix point");
      ExpectVec3(out_n, TransformVector(palette[3], {1, 0, 0}), 1e-4,
                 "single influence reconstructs matrix normal");
      ExpectVec3(out_t, TransformVector(palette[3], {0, 1, 0}), 1e-4,
                 "single influence reconstructs matrix tangent");
    }
  }

  // Equal weights and palette indices 3 versus 1: reference must be palette 1.
  // Final transform is sign-invariant, but this exercises the required tie
  // branch and fixed sign path.
  const uint16_t tie_indices[2] = {3, 1};
  float tie_weight = 0.5f;
  float p[3] = {0.4f, 0.2f, -0.1f};
  float out[3] = {};
  ozz::geometry::SkinningJob job;
  job.vertex_count = 1;
  job.influences_count = 2;
  job.skinning_mode = ozz::geometry::SkinningJob::kDualQuaternion;
  job.joint_matrices =
      ozz::span<ozz::math::Float4x4>(ozz_palette.data(), ozz_palette.size());
  job.joint_indices = ozz::make_span(tie_indices);
  job.joint_indices_stride = sizeof(tie_indices);
  job.joint_weights = ozz::span<float>(&tie_weight, 1);
  job.joint_weights_stride = sizeof(float);
  job.in_positions = p;
  job.in_positions_stride = sizeof(p);
  job.out_positions = out;
  job.out_positions_stride = sizeof(out);
  Check(job.Run(), "tie run");
  const Skinned e =
      ExpectedDQ(palette, {3, 1}, {0.5}, {0.4, 0.2, -0.1}, {}, {});
  ExpectVec3(out, e.position, 1e-4, "tie coordinate");
}

std::vector<M4> MakeMixedPalette() {
  const double pi = std::acos(-1.0);
  std::vector<M4> p;
  p.push_back(Transform(AxisAngle({0, 0, 1}, 95 * pi / 180), {0.1, 0.2, 0}));
  p.push_back(Transform(AxisAngle({0, 0, 1}, -95 * pi / 180), {-0.2, 0.1, 0.3}));
  p.push_back(Transform(Q{}, {0.4, 0, 0}, {2.0, 2.0, 2.0}));  // uniform scale
  p.push_back(Transform(AxisAngle({0, 1, 0}, 0.4), {}, {1.7, 0.6, 1.2}));
  p.push_back(Transform(Q{}, {}, {1, 1, -1}));  // reflection
  M4 shear = Transform(AxisAngle({0.3, 1, 0.2}, 0.7), {0.2, -0.1, 0.1});
  shear.m[0][1] += 0.8;
  p.push_back(shear);
  return p;
}

void TestMixedRigidAndLinearVertices() {
  constexpr int kVertices = 6;
  constexpr int kInfluences = 3;
  const std::vector<M4> m4_palette = MakeMixedPalette();
  std::vector<ozz::math::Float4x4> palette, it_palette;
  for (const M4& m : m4_palette) {
    palette.push_back(ToOzz(m));
    it_palette.push_back(ToOzz(Transpose(Inverse(m))));
  }

  uint16_t indices[kVertices * kInfluences] = {
      0, 1, 0,  // rigid
      0, 2, 1,  // uniform scale
      3, 1, 0,  // non-uniform scale
      0, 4, 1,  // reflection
      5, 1, 0,  // shear
      0, 1, 4   // zero-weight non-rigid last influence
  };
  float weights[kVertices * 2] = {
      0.5f, 0.0f,  // restored .5
      0.4f, 0.3f,  // restored .3
      0.7f, 0.1f,  // restored .2
      0.4f, 0.2f,  // restored .4
      0.2f, 0.3f,  // restored .5
      0.5f, 0.5f   // restored zero
  };

  std::vector<float> in_p(kVertices * 3), in_n(kVertices * 3),
      in_t(kVertices * 3);
  for (int v = 0; v < kVertices; ++v) {
    in_p[v * 3 + 0] = 0.1f + 0.07f * v;
    in_p[v * 3 + 1] = -0.3f + 0.05f * v;
    in_p[v * 3 + 2] = 0.22f;
    in_n[v * 3 + 0] = 0.0f;
    in_n[v * 3 + 1] = 1.0f;
    in_n[v * 3 + 2] = 0.0f;
    in_t[v * 3 + 0] = 1.0f;
    in_t[v * 3 + 1] = 0.0f;
    in_t[v * 3 + 2] = 0.0f;
  }
  std::vector<float> dq_p(kVertices * 3, 55.f), dq_n(kVertices * 3, 55.f),
      dq_t(kVertices * 3, 55.f);
  std::vector<float> lbs_p(kVertices * 3, 56.f), lbs_n(kVertices * 3, 56.f),
      lbs_t(kVertices * 3, 56.f);

  ozz::geometry::SkinningJob job;
  job.vertex_count = kVertices;
  job.influences_count = kInfluences;
  job.skinning_mode = ozz::geometry::SkinningJob::kDualQuaternion;
  job.joint_matrices = ozz::span<ozz::math::Float4x4>(palette.data(),
                                                       palette.size());
  job.joint_inverse_transpose_matrices = ozz::span<ozz::math::Float4x4>(
      it_palette.data(), it_palette.size());
  job.joint_indices = ozz::make_span(indices);
  job.joint_indices_stride = kInfluences * sizeof(uint16_t);
  job.joint_weights = ozz::make_span(weights);
  job.joint_weights_stride = 2 * sizeof(float);
  job.in_positions = FloatSpan(in_p);
  job.in_positions_stride = 3 * sizeof(float);
  job.out_positions = MutableFloatSpan(dq_p);
  job.out_positions_stride = 3 * sizeof(float);
  job.in_normals = FloatSpan(in_n);
  job.in_normals_stride = 3 * sizeof(float);
  job.out_normals = MutableFloatSpan(dq_n);
  job.out_normals_stride = 3 * sizeof(float);
  job.in_tangents = FloatSpan(in_t);
  job.in_tangents_stride = 3 * sizeof(float);
  job.out_tangents = MutableFloatSpan(dq_t);
  job.out_tangents_stride = 3 * sizeof(float);
  Check(job.Run(), "mixed DQ/LBS run");

  ozz::geometry::SkinningJob lbs = job;
  lbs.skinning_mode = ozz::geometry::SkinningJob::kLinear;
  lbs.out_positions = MutableFloatSpan(lbs_p);
  lbs.out_normals = MutableFloatSpan(lbs_n);
  lbs.out_tangents = MutableFloatSpan(lbs_t);
  Check(lbs.Run(), "mixed linear reference run");

  for (int v = 1; v <= 4; ++v) {
    for (int k = 0; k < 3; ++k) {
      char msg[160];
      std::snprintf(msg, sizeof(msg), "fallback vertex %d position %d", v, k);
      Near(dq_p[v * 3 + k], lbs_p[v * 3 + k], 1e-5, msg);
      std::snprintf(msg, sizeof(msg), "fallback vertex %d normal %d", v, k);
      Near(dq_n[v * 3 + k], lbs_n[v * 3 + k], 1e-5, msg);
      std::snprintf(msg, sizeof(msg), "fallback vertex %d tangent %d", v, k);
      Near(dq_t[v * 3 + k], lbs_t[v * 3 + k], 1e-5, msg);
    }
  }

  // Vertex zero and the zero-weight-non-rigid vertex must follow independent
  // DQ calculations.
  const V3 p0{in_p[0], in_p[1], in_p[2]};
  const Skinned e0 = ExpectedDQ(m4_palette, {0, 1, 0}, {0.5, 0.0}, p0,
                                {0, 1, 0}, {1, 0, 0});
  ExpectVec3(&dq_p[0], e0.position, 1e-4, "mixed rigid position");
  ExpectVec3(&dq_n[0], e0.normal, 1e-4, "mixed rigid normal");
  ExpectVec3(&dq_t[0], e0.tangent, 1e-4, "mixed rigid tangent");

  const V3 p5{in_p[15], in_p[16], in_p[17]};
  const Skinned e5 = ExpectedDQ(m4_palette, {0, 1, 4}, {0.5, 0.5}, p5,
                                {0, 1, 0}, {1, 0, 0});
  ExpectVec3(&dq_p[15], e5.position, 1e-4, "zero nonrigid position");
  ExpectVec3(&dq_n[15], e5.normal, 1e-4, "zero nonrigid normal");

  // For non-rigid vertices, the correct LBS result must also be measurably
  // different from blindly feeding the non-rigid matrices through DQ. This
  // keeps the fallback branch observable in the test fixture.
  for (int v = 1; v <= 4; ++v) {
    const std::vector<uint16_t> ix(indices + v * 3, indices + v * 3 + 3);
    const std::vector<double> ew{weights[v * 2 + 0], weights[v * 2 + 1]};
    const V3 p{in_p[v * 3 + 0], in_p[v * 3 + 1], in_p[v * 3 + 2]};
    const Skinned pseudo_dq =
        ExpectedDQ(m4_palette, ix, ew, p, {0, 1, 0}, {1, 0, 0});
    const bool looks_like_dq =
        Vec3Near(&dq_p[v * 3], pseudo_dq.position, 1e-3) &&
        Vec3Near(&dq_n[v * 3], pseudo_dq.normal, 1e-3);
    Check(!looks_like_dq, "non-rigid vertex does not take the DQ path");
  }
}

void TestInterleavedInPlace() {
  struct Vertex {
    float position[3];
    uint32_t color;
    float normal[3];
    float uv[2];
    float tangent[4];
    float padding;
  };
  static_assert(sizeof(Vertex) == 56, "unexpected interleaved vertex size");

  Vertex vertices[2] = {};
  for (int v = 0; v < 2; ++v) {
    vertices[v].position[0] = 0.2f + v;
    vertices[v].position[1] = -0.35f;
    vertices[v].position[2] = 0.45f;
    vertices[v].color = 0xc0ffee00u + v;
    vertices[v].normal[0] = 0;
    vertices[v].normal[1] = 0;
    vertices[v].normal[2] = 1;
    vertices[v].uv[0] = 3.25f + v;
    vertices[v].uv[1] = 7.5f;
    vertices[v].tangent[0] = 1;
    vertices[v].tangent[1] = 0;
    vertices[v].tangent[2] = 0;
    vertices[v].tangent[3] = 0.875f;
    vertices[v].padding = -9.25f;
  }

  std::vector<M4> m4_palette;
  m4_palette.push_back(
      Transform(AxisAngle({0, 0, 1}, 0.8), {0.2, -0.1, 0.05}));
  m4_palette.push_back(
      Transform(AxisAngle({1, 0, 0}, -0.6), {-0.1, 0.3, 0.0}));
  m4_palette.push_back(
      Transform(AxisAngle({0, 1, 0}, 0.45), {0.0, 0.0, 0.2}));
  m4_palette.push_back(
      Transform(AxisAngle({1, 1, 1}, 0.35), {0.1, 0.1, -0.1}));
  std::vector<ozz::math::Float4x4> palette;
  for (const M4& m : m4_palette) palette.push_back(ToOzz(m));

  uint16_t indices[8] = {3, 1, 0, 2, 0, 2, 1, 3};
  float weights[6] = {0.2f, 0.3f, 0.1f, 0.4f, 0.1f, 0.2f};

  const size_t total_bytes = sizeof(vertices);
  const size_t normal_bytes =
      total_bytes - offsetof(Vertex, normal);
  const size_t tangent_bytes =
      total_bytes - offsetof(Vertex, tangent);

  ozz::geometry::SkinningJob job;
  job.vertex_count = 2;
  job.influences_count = 4;
  job.skinning_mode = ozz::geometry::SkinningJob::kDualQuaternion;
  job.joint_matrices =
      ozz::span<ozz::math::Float4x4>(palette.data(), palette.size());
  job.joint_indices = ozz::make_span(indices);
  job.joint_indices_stride = 4 * sizeof(uint16_t);
  job.joint_weights = ozz::make_span(weights);
  job.joint_weights_stride = 3 * sizeof(float);

  job.in_positions = ozz::span<const float>(
      reinterpret_cast<const float*>(vertices), total_bytes / sizeof(float));
  job.out_positions = ozz::span<float>(reinterpret_cast<float*>(vertices),
                                       total_bytes / sizeof(float));
  job.in_positions_stride = sizeof(Vertex);
  job.out_positions_stride = sizeof(Vertex);

  job.in_normals = ozz::span<const float>(
      reinterpret_cast<const float*>(&vertices[0].normal[0]),
      normal_bytes / sizeof(float));
  job.out_normals = ozz::span<float>(
      reinterpret_cast<float*>(&vertices[0].normal[0]),
      normal_bytes / sizeof(float));
  job.in_normals_stride = sizeof(Vertex);
  job.out_normals_stride = sizeof(Vertex);

  job.in_tangents = ozz::span<const float>(
      reinterpret_cast<const float*>(&vertices[0].tangent[0]),
      tangent_bytes / sizeof(float));
  job.out_tangents = ozz::span<float>(
      reinterpret_cast<float*>(&vertices[0].tangent[0]),
      tangent_bytes / sizeof(float));
  job.in_tangents_stride = sizeof(Vertex);
  job.out_tangents_stride = sizeof(Vertex);

  Check(job.Validate() && job.Run(), "interleaved in-place run");

  for (int v = 0; v < 2; ++v) {
    Check(vertices[v].color == 0xc0ffee00u + v, "color preserved");
    Near(vertices[v].uv[0], 3.25 + v, 0.0, "uv u preserved");
    Near(vertices[v].uv[1], 7.5, 0.0, "uv v preserved");
    Near(vertices[v].tangent[3], 0.875, 0.0, "tangent w preserved");
    Near(vertices[v].padding, -9.25, 0.0, "padding preserved");

    const V3 p{0.2 + v, -0.35, 0.45};
    const V3 expected_n{0, 0, 1};
    const V3 expected_t{1, 0, 0};
    const std::vector<uint16_t> ix{indices[v * 4 + 0], indices[v * 4 + 1],
                                   indices[v * 4 + 2], indices[v * 4 + 3]};
    const std::vector<double> ew{weights[v * 3 + 0], weights[v * 3 + 1],
                                 weights[v * 3 + 2]};
    const Skinned e = ExpectedDQ(m4_palette, ix, ew, p, expected_n, expected_t);
    ExpectVec3(vertices[v].position, e.position, 1e-4,
               "interleaved position");
    ExpectVec3(vertices[v].normal, e.normal, 1e-4, "interleaved normal");
    ExpectVec3(vertices[v].tangent, e.tangent, 1e-4, "interleaved tangent");
  }
}

void TestInvalidLastVertexIsAtomic(bool bad_index, bool zero_weight_bad_index,
                                   bool bad_weight) {
  constexpr int kVertices = 3;
  constexpr int kInfluences = 2;
  ozz::math::Float4x4 palette[2] = {ozz::math::Float4x4::identity(),
                                    ozz::math::Float4x4::identity()};
  uint16_t indices[kVertices * kInfluences] = {};
  float weights[kVertices] = {0.5f, 0.5f, 0.5f};
  if (bad_index) {
    indices[(kVertices - 1) * 2 + 1] = 9;
    if (zero_weight_bad_index) weights[kVertices - 1] = 1.0f;
  }
  if (bad_weight) {
    weights[kVertices - 1] = std::numeric_limits<float>::quiet_NaN();
  }

  std::vector<float> p(kVertices * 3, 3.25f);
  std::vector<float> n(kVertices * 3, 4.25f);
  std::vector<float> t(kVertices * 3, 5.25f);
  const std::vector<float> before_p = p;
  const std::vector<float> before_n = n;
  const std::vector<float> before_t = t;

  ozz::geometry::SkinningJob job;
  job.vertex_count = kVertices;
  job.influences_count = kInfluences;
  job.skinning_mode = ozz::geometry::SkinningJob::kDualQuaternion;
  job.joint_matrices = palette;
  job.joint_indices = ozz::make_span(indices);
  job.joint_indices_stride = kInfluences * sizeof(uint16_t);
  job.joint_weights = ozz::make_span(weights);
  job.joint_weights_stride = sizeof(float);
  job.in_positions = FloatSpan(before_p);
  job.in_positions_stride = 3 * sizeof(float);
  job.out_positions = MutableFloatSpan(p);
  job.out_positions_stride = 3 * sizeof(float);
  job.in_normals = FloatSpan(before_n);
  job.in_normals_stride = 3 * sizeof(float);
  job.out_normals = MutableFloatSpan(n);
  job.out_normals_stride = 3 * sizeof(float);
  job.in_tangents = FloatSpan(before_t);
  job.in_tangents_stride = 3 * sizeof(float);
  job.out_tangents = MutableFloatSpan(t);
  job.out_tangents_stride = 3 * sizeof(float);

  Check(job.Validate(), "invalid fixture still passes span validation");
  Check(!job.Run(), "DQ rejects invalid final vertex");
  Check(p == before_p, "invalid run leaves positions untouched");
  Check(n == before_n, "invalid run leaves normals untouched");
  Check(t == before_t, "invalid run leaves tangents untouched");
}

void TestOldModeAllowsOutOfRangeWeights() {
  ozz::math::Float4x4 palette[1] = {ozz::math::Float4x4::identity()};
  uint16_t indices[2] = {0, 0};
  float weight = 1.25f;  // Restored last weight is -0.25.
  float in[3] = {1, 2, 3};
  float out[3] = {};
  ozz::geometry::SkinningJob job;
  job.vertex_count = 1;
  job.influences_count = 2;
  job.skinning_mode = ozz::geometry::SkinningJob::kLinear;
  job.joint_matrices = palette;
  job.joint_indices = indices;
  job.joint_indices_stride = sizeof(indices);
  job.joint_weights = ozz::span<float>(&weight, 1);
  job.joint_weights_stride = sizeof(float);
  job.in_positions = in;
  job.in_positions_stride = sizeof(in);
  job.out_positions = out;
  job.out_positions_stride = sizeof(out);
  Check(job.Validate() && job.Run(), "old mode keeps permissive weight data");
}

}  // namespace

int main() {
  TestLargeTwist();
  TestTranslationsAndDifferentAxes();
  TestInfluenceCountsAndTie();
  TestMixedRigidAndLinearVertices();
  TestInterleavedInPlace();
  TestInvalidLastVertexIsAtomic(true, false, false);
  TestInvalidLastVertexIsAtomic(true, true, false);
  TestInvalidLastVertexIsAtomic(false, false, true);
  TestOldModeAllowsOutOfRangeWeights();

  if (g_failures != 0) {
    std::printf("OZZ_DQ_TARGET_FAILED failures=%d\n", g_failures);
    return 1;
  }
  std::printf("OZZ_DQ_TARGET_OK\n");
  return 0;
}
