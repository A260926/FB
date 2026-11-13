//----------------------------------------------------------------------------//
// Dual quaternion skinning target test.                                      //
//                                                                            //
// Validates the new SkinningJob::kDualQuaternion mode against an independent  //
// scalar dual quaternion / matrix implementation. The reference implementation//
// is written from the specification, without calling any of the job's internal//
// helpers.                                                                   //
//                                                                            //
// Prints OZZ_DQ_TARGET_OK on success, exits with a non-zero status on failure //
// (diagnostics are printed on stderr).                                       //
//----------------------------------------------------------------------------//

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <vector>

#include "ozz/base/maths/simd_math.h"
#include "ozz/base/maths/vec_float.h"
#include "ozz/base/span.h"
#include "ozz/geometry/runtime/skinning_job.h"

namespace {

int g_failures = 0;

void Fail(const char* _file, int _line, const char* _what) {
  std::fprintf(stderr, "%s:%d: check failed: %s\n", _file, _line, _what);
  ++g_failures;
}

#define CHECK(_condition)                                    \
  do {                                                       \
    if (!(_condition)) {                                     \
      Fail(__FILE__, __LINE__, #_condition);                 \
    }                                                        \
  } while (0)

void CheckClose(float _actual, float _expected, float _tolerance,
                const char* _what) {
  const float error = std::fabs(_actual - _expected);
  if (!(error <= _tolerance)) {
    std::fprintf(stderr, "check failed: %s actual=%g expected=%g error=%g > %g\n",
                 _what, static_cast<double>(_actual),
                 static_cast<double>(_expected), static_cast<double>(error),
                 static_cast<double>(_tolerance));
    ++g_failures;
  }
}

void CheckVec3(const float* _actual, const float* _expected, float _tolerance,
               const char* _what) {
  char label[256];
  for (int i = 0; i < 3; ++i) {
    std::snprintf(label, sizeof(label), "%s[%d]", _what, i);
    CheckClose(_actual[i], _expected[i], _tolerance, label);
  }
}

// Returns the matrix component at column _c, row _r.
float M(const ozz::math::Float4x4& _m, int _c, int _r) {
  switch (_r) {
    case 0:
      return ozz::math::GetX(_m.cols[_c]);
    case 1:
      return ozz::math::GetY(_m.cols[_c]);
    case 2:
      return ozz::math::GetZ(_m.cols[_c]);
    default:
      return ozz::math::GetW(_m.cols[_c]);
  }
}

ozz::math::Float4x4 MakeMatrix(float _c0x, float _c0y, float _c0z, float _c0w,
                               float _c1x, float _c1y, float _c1z, float _c1w,
                               float _c2x, float _c2y, float _c2z, float _c2w,
                               float _c3x, float _c3y, float _c3z,
                               float _c3w) {
  ozz::math::Float4x4 m;
  m.cols[0] = ozz::math::simd_float4::Load(_c0x, _c0y, _c0z, _c0w);
  m.cols[1] = ozz::math::simd_float4::Load(_c1x, _c1y, _c1z, _c1w);
  m.cols[2] = ozz::math::simd_float4::Load(_c2x, _c2y, _c2z, _c2w);
  m.cols[3] = ozz::math::simd_float4::Load(_c3x, _c3y, _c3z, _c3w);
  return m;
}

ozz::math::Float4x4 Identity() { return ozz::math::Float4x4::identity(); }

// Builds a rigid matrix from a unit axis, an angle (radians) and a translation.
ozz::math::Float4x4 Rigid(float _ax, float _ay, float _az, float _angle,
                          float _tx, float _ty, float _tz) {
  ozz::math::Float4x4 m = ozz::math::Float4x4::FromAxisAngle(
      ozz::math::simd_float4::Load(_ax, _ay, _az, 0.f),
      ozz::math::simd_float4::Load1(_angle));
  m.cols[3] = ozz::math::simd_float4::Load(_tx, _ty, _tz, 1.f);
  return m;
}

ozz::math::Float4x4 RigidX(float _angle, float _tx, float _ty, float _tz) {
  return Rigid(1.f, 0.f, 0.f, _angle, _tx, _ty, _tz);
}
ozz::math::Float4x4 RigidY(float _angle, float _tx, float _ty, float _tz) {
  return Rigid(0.f, 1.f, 0.f, _angle, _tx, _ty, _tz);
}
ozz::math::Float4x4 RigidZ(float _angle, float _tx, float _ty, float _tz) {
  return Rigid(0.f, 0.f, 1.f, _angle, _tx, _ty, _tz);
}

ozz::math::Float4x4 Scaling(float _x, float _y, float _z) {
  return ozz::math::Float4x4::Scaling(ozz::math::simd_float4::Load(_x, _y, _z, 1.f));
}

//============================================================================//
// Independent scalar reference implementation.                               //
//============================================================================//

namespace ref {

const float kRigidTolerance = 1e-4f;
const float kRealPartLengthTolerance = 1e-6f;

float C(const ozz::math::Float4x4& _m, int _c, int _r) {
  return M(_m, _c, _r);
}

bool Finite(const ozz::math::Float4x4& _m) {
  for (int c = 0; c < 4; ++c) {
    for (int r = 0; r < 4; ++r) {
      if (!std::isfinite(C(_m, c, r))) {
        return false;
      }
    }
  }
  return true;
}

bool IsRigid(const ozz::math::Float4x4& _m) {
  if (!Finite(_m)) {
    return false;
  }
  // Bottom row.
  if (std::fabs(C(_m, 0, 3)) > kRigidTolerance ||
      std::fabs(C(_m, 1, 3)) > kRigidTolerance ||
      std::fabs(C(_m, 2, 3)) > kRigidTolerance ||
      std::fabs(C(_m, 3, 3) - 1.f) > kRigidTolerance) {
    return false;
  }
  float cols[3][3];
  for (int c = 0; c < 3; ++c) {
    cols[c][0] = C(_m, c, 0);
    cols[c][1] = C(_m, c, 1);
    cols[c][2] = C(_m, c, 2);
    const float len = std::sqrt(cols[c][0] * cols[c][0] +
                                cols[c][1] * cols[c][1] +
                                cols[c][2] * cols[c][2]);
    if (std::fabs(len - 1.f) > kRigidTolerance) {
      return false;
    }
  }
  for (int a = 0; a < 3; ++a) {
    for (int b = a + 1; b < 3; ++b) {
      const float dot = cols[a][0] * cols[b][0] + cols[a][1] * cols[b][1] +
                        cols[a][2] * cols[b][2];
      if (std::fabs(dot) > kRigidTolerance) {
        return false;
      }
    }
  }
  const float det =
      cols[0][0] * (cols[1][1] * cols[2][2] - cols[1][2] * cols[2][1]) -
      cols[1][0] * (cols[0][1] * cols[2][2] - cols[0][2] * cols[2][1]) +
      cols[2][0] * (cols[0][1] * cols[1][2] - cols[0][2] * cols[1][1]);
  if (std::fabs(det - 1.f) > kRigidTolerance) {
    return false;
  }
  return true;
}

struct Q {
  float x, y, z, w;
};

// Extracts the rotation quaternion, normalized, sign fixed in w, z, y, x order.
bool Rotation(const ozz::math::Float4x4& _m, Q* _q) {
  const float r00 = C(_m, 0, 0), r10 = C(_m, 0, 1), r20 = C(_m, 0, 2);
  const float r01 = C(_m, 1, 0), r11 = C(_m, 1, 1), r21 = C(_m, 1, 2);
  const float r02 = C(_m, 2, 0), r12 = C(_m, 2, 1), r22 = C(_m, 2, 2);
  const float trace = r00 + r11 + r22;
  float x, y, z, w;
  if (trace > 0.f) {
    const float s = std::sqrt(trace + 1.f) * 2.f;
    w = 0.25f * s;
    x = (r21 - r12) / s;
    y = (r02 - r20) / s;
    z = (r10 - r01) / s;
  } else if (r00 > r11 && r00 > r22) {
    const float s = std::sqrt(1.f + r00 - r11 - r22) * 2.f;
    x = 0.25f * s;
    w = (r21 - r12) / s;
    y = (r01 + r10) / s;
    z = (r02 + r20) / s;
  } else if (r11 > r22) {
    const float s = std::sqrt(1.f + r11 - r00 - r22) * 2.f;
    y = 0.25f * s;
    w = (r02 - r20) / s;
    x = (r01 + r10) / s;
    z = (r12 + r21) / s;
  } else {
    const float s = std::sqrt(1.f + r22 - r00 - r11) * 2.f;
    z = 0.25f * s;
    w = (r10 - r01) / s;
    x = (r02 + r20) / s;
    y = (r12 + r21) / s;
  }
  const float len_sqr = x * x + y * y + z * z + w * w;
  if (!(len_sqr >= kRealPartLengthTolerance * kRealPartLengthTolerance)) {
    return false;
  }
  const float inv = 1.f / std::sqrt(len_sqr);
  x *= inv;
  y *= inv;
  z *= inv;
  w *= inv;
  float first = w;
  if (first == 0.f) first = z;
  if (first == 0.f) first = y;
  if (first == 0.f) first = x;
  if (first < 0.f) {
    x = -x;
    y = -y;
    z = -z;
    w = -w;
  }
  _q->x = x;
  _q->y = y;
  _q->z = z;
  _q->w = w;
  return true;
}

Q Mul(const Q& _a, const Q& _b) {
  Q r;
  r.x = _a.w * _b.x + _a.x * _b.w + _a.y * _b.z - _a.z * _b.y;
  r.y = _a.w * _b.y + _a.y * _b.w + _a.z * _b.x - _a.x * _b.z;
  r.z = _a.w * _b.z + _a.z * _b.w + _a.x * _b.y - _a.y * _b.x;
  r.w = _a.w * _b.w - _a.x * _b.x - _a.y * _b.y - _a.z * _b.z;
  return r;
}

void Rotate(const Q& _q, const float _v[3], float _out[3]) {
  const float tx = 2.f * (_q.y * _v[2] - _q.z * _v[1]);
  const float ty = 2.f * (_q.z * _v[0] - _q.x * _v[2]);
  const float tz = 2.f * (_q.x * _v[1] - _q.y * _v[0]);
  _out[0] = _v[0] + _q.w * tx + (_q.y * tz - _q.z * ty);
  _out[1] = _v[1] + _q.w * ty + (_q.z * tx - _q.x * tz);
  _out[2] = _v[2] + _q.w * tz + (_q.x * ty - _q.y * tx);
}

struct Result {
  bool dual = false;
  float position[3] = {0.f, 0.f, 0.f};
  float normal[3] = {0.f, 0.f, 0.f};
  float tangent[3] = {0.f, 0.f, 0.f};
};

// Skin one vertex. _matrices is the palette, _it optional inverse transpose
// palette (nullptr when not provided). _indices has _influences entries,
// _weights has _influences - 1 entries (the last one is restored).
Result Skin(const ozz::math::Float4x4* _matrices,
            const ozz::math::Float4x4* _it_matrices, const uint16_t* _indices,
            int _influences, const float* _weights, const float _position[3],
            const float* _normal, const float* _tangent) {
  Result result;
  const int explicit_weights = _influences - 1;
  float weight_sum = 0.f;
  for (int j = 0; j < explicit_weights; ++j) {
    weight_sum += _weights[j];
  }
  auto weight_at = [explicit_weights, _weights, weight_sum](int _j) {
    return _j < explicit_weights ? _weights[_j] : 1.f - weight_sum;
  };

  // Reference influence.
  int reference = -1;
  float reference_weight = -1.f;
  for (int j = 0; j < _influences; ++j) {
    const float w = weight_at(j);
    if (w <= 0.f) {
      continue;
    }
    const uint16_t index = _indices[j];
    if (reference < 0 || w > reference_weight ||
        (w == reference_weight && index < _indices[reference])) {
      reference = j;
      reference_weight = w;
    }
  }
  const uint16_t reference_index = _indices[reference];

  Q reference_q;
  bool dual = IsRigid(_matrices[reference_index]) &&
              Rotation(_matrices[reference_index], &reference_q);

  float rx = 0.f, ry = 0.f, rz = 0.f, rw = 0.f;
  float dx = 0.f, dy = 0.f, dz = 0.f, dw = 0.f;
  for (int j = 0; dual && j < _influences; ++j) {
    const float w = weight_at(j);
    if (w == 0.f) {
      continue;
    }
    const uint16_t index = _indices[j];
    const ozz::math::Float4x4& m = _matrices[index];
    if (!IsRigid(m)) {
      dual = false;
      break;
    }
    Q q;
    if (!Rotation(m, &q)) {
      dual = false;
      break;
    }
    // dual = 0.5 * (0, t) * real
    const Q t = {C(m, 3, 0), C(m, 3, 1), C(m, 3, 2), 0.f};
    const Q td = Mul(t, q);
    float dqx = 0.5f * td.x;
    float dqy = 0.5f * td.y;
    float dqz = 0.5f * td.z;
    float dqw = 0.5f * td.w;
    const float dot = q.x * reference_q.x + q.y * reference_q.y +
                      q.z * reference_q.z + q.w * reference_q.w;
    if (dot < 0.f) {
      q.x = -q.x;
      q.y = -q.y;
      q.z = -q.z;
      q.w = -q.w;
      dqx = -dqx;
      dqy = -dqy;
      dqz = -dqz;
      dqw = -dqw;
    }
    rx += w * q.x;
    ry += w * q.y;
    rz += w * q.z;
    rw += w * q.w;
    dx += w * dqx;
    dy += w * dqy;
    dz += w * dqz;
    dw += w * dqw;
  }

  if (dual) {
    const float len = std::sqrt(rx * rx + ry * ry + rz * rz + rw * rw);
    if (!(len >= kRealPartLengthTolerance)) {
      dual = false;
    } else {
      const float inv = 1.f / len;
      const Q real = {rx * inv, ry * inv, rz * inv, rw * inv};
      const float real_dot_dual =
          real.x * dx + real.y * dy + real.z * dz + real.w * dw;
      const Q d = {(dx - real.x * real_dot_dual) * inv,
                   (dy - real.y * real_dot_dual) * inv,
                   (dz - real.z * real_dot_dual) * inv,
                   (dw - real.w * real_dot_dual) * inv};
      // Translation = 2 * (dual * conjugate(real)).
      const Q t = Mul(d, {-real.x, -real.y, -real.z, real.w});
      result.position[0] = 2.f * t.x;
      result.position[1] = 2.f * t.y;
      result.position[2] = 2.f * t.z;
      float rotated[3];
      Rotate(real, _position, rotated);
      result.position[0] += rotated[0];
      result.position[1] += rotated[1];
      result.position[2] += rotated[2];
      if (_normal) {
        Rotate(real, _normal, result.normal);
      }
      if (_tangent) {
        Rotate(real, _tangent, result.tangent);
      }
      result.dual = true;
      return result;
    }
  }

  // Linear blend fallback, identical to the legacy linear mode formula.
  float m[4][4];
  float it[4][4];
  std::memset(m, 0, sizeof(m));
  std::memset(it, 0, sizeof(it));
  for (int j = 0; j < _influences; ++j) {
    const float w = weight_at(j);
    const uint16_t index = _indices[j];
    for (int c = 0; c < 4; ++c) {
      for (int r = 0; r < 4; ++r) {
        m[c][r] += C(_matrices[index], c, r) * w;
        it[c][r] += (_it_matrices ? C(_it_matrices[index], c, r)
                                  : C(_matrices[index], c, r)) *
                    w;
      }
    }
  }
  for (int i = 0; i < 3; ++i) {
    result.position[i] = m[0][i] * _position[0] + m[1][i] * _position[1] +
                         m[2][i] * _position[2] + m[3][i];
  }
  if (_normal) {
    for (int i = 0; i < 3; ++i) {
      result.normal[i] = it[0][i] * _normal[0] + it[1][i] * _normal[1] +
                         it[2][i] * _normal[2];
    }
  }
  if (_tangent) {
    for (int i = 0; i < 3; ++i) {
      result.tangent[i] = it[0][i] * _tangent[0] + it[1][i] * _tangent[1] +
                          it[2][i] * _tangent[2];
    }
  }
  result.dual = false;
  return result;
}

}  // namespace ref

//============================================================================//
// Job helpers.                                                               //
//============================================================================//

using ozz::geometry::SkinningJob;

// Runs a one-vertex-at-a-time job with tightly packed (or explicitly strided)
// buffers. Returns the outputs in vectors.
struct SimpleMesh {
  std::vector<uint16_t> indices;
  std::vector<float> weights;
  std::vector<float> positions;
  std::vector<float> normals;
  std::vector<float> tangents;
  int vertex_count = 0;
  int influences = 0;

  void Resize(int _vertices, int _influences, bool _normals, bool _tangents) {
    vertex_count = _vertices;
    influences = _influences;
    indices.assign(static_cast<size_t>(_vertices) * _influences, 0);
    weights.assign(static_cast<size_t>(_vertices) * (_influences - 1), 0.f);
    positions.assign(static_cast<size_t>(_vertices) * 3, 0.f);
    normals.assign(_normals ? static_cast<size_t>(_vertices) * 3 : 0, 0.f);
    tangents.assign(_tangents ? static_cast<size_t>(_vertices) * 3 : 0, 0.f);
  }
};

// Fills a job that reads/writes SimpleMesh densely.
SkinningJob MakeJob(const SimpleMesh& _mesh,
                    const std::vector<ozz::math::Float4x4>& _matrices,
                    const std::vector<ozz::math::Float4x4>* _it_matrices,
                    float* _out_positions, float* _out_normals,
                    float* _out_tangents, SkinningJob::SkinningMode _mode) {
  SkinningJob job;
  job.vertex_count = _mesh.vertex_count;
  job.mode = _mode;
  job.influences_count = _mesh.influences;
  job.joint_matrices = {_matrices.data(), _matrices.size()};
  if (_it_matrices) {
    job.joint_inverse_transpose_matrices = {_it_matrices->data(),
                                            _it_matrices->size()};
  }
  job.joint_indices = {_mesh.indices.data(), _mesh.indices.size()};
  job.joint_indices_stride = sizeof(uint16_t) * _mesh.influences;
  if (_mesh.influences > 1) {
    job.joint_weights = {_mesh.weights.data(), _mesh.weights.size()};
    job.joint_weights_stride = sizeof(float) * (_mesh.influences - 1);
  }
  job.in_positions = {_mesh.positions.data(), _mesh.positions.size()};
  job.in_positions_stride = sizeof(float) * 3;
  job.out_positions = {_out_positions, _mesh.positions.size()};
  job.out_positions_stride = sizeof(float) * 3;
  if (!_mesh.normals.empty() && _out_normals != nullptr) {
    job.in_normals = {_mesh.normals.data(), _mesh.normals.size()};
    job.in_normals_stride = sizeof(float) * 3;
    job.out_normals = {_out_normals, _mesh.normals.size()};
    job.out_normals_stride = sizeof(float) * 3;
  }
  if (!_mesh.tangents.empty() && _out_tangents != nullptr) {
    job.in_tangents = {_mesh.tangents.data(), _mesh.tangents.size()};
    job.in_tangents_stride = sizeof(float) * 3;
    job.out_tangents = {_out_tangents, _mesh.tangents.size()};
    job.out_tangents_stride = sizeof(float) * 3;
  }
  return job;
}

//============================================================================//
// Tests.                                                                     //
//============================================================================//

// Positive and negative large-angle twists: the cross-section radius must be
// preserved, unlike linear blend skinning which squashes it.
void TestLargeTwistRadius() {
  const float angle = 100.f * 3.14159265358979323846f / 180.f;
  std::vector<ozz::math::Float4x4> matrices;
  matrices.push_back(RigidX(+angle, 0.f, 0.f, 0.f));
  matrices.push_back(RigidX(-angle, 0.f, 0.f, 0.f));

  const int vertex_count = 8;
  SimpleMesh mesh;
  mesh.Resize(vertex_count, 2, true, true);
  for (int v = 0; v < vertex_count; ++v) {
    const float phi = static_cast<float>(v) * 2.f * 3.14159265358979323846f /
                      static_cast<float>(vertex_count);
    mesh.indices[v * 2 + 0] = 0;
    mesh.indices[v * 2 + 1] = 1;
    mesh.weights[v] = .5f;
    mesh.positions[v * 3 + 0] = 0.f;
    mesh.positions[v * 3 + 1] = 0.6f * std::cos(phi);
    mesh.positions[v * 3 + 2] = 0.6f * std::sin(phi);
    mesh.normals[v * 3 + 0] = 1.f;
    mesh.normals[v * 3 + 1] = 0.f;
    mesh.normals[v * 3 + 2] = 0.f;
    mesh.tangents[v * 3 + 0] = 0.f;
    mesh.tangents[v * 3 + 1] = -std::sin(phi);
    mesh.tangents[v * 3 + 2] = std::cos(phi);
  }

  std::vector<float> dq_pos(mesh.positions.size()), dq_nrm(mesh.normals.size()),
      dq_tan(mesh.tangents.size());
  std::vector<float> lin_pos(mesh.positions.size());

  SkinningJob dq = MakeJob(mesh, matrices, nullptr, dq_pos.data(),
                           dq_nrm.data(), dq_tan.data(),
                           SkinningJob::kDualQuaternion);
  CHECK(dq.Run());
  SkinningJob linear = MakeJob(mesh, matrices, nullptr, lin_pos.data(), nullptr,
                               nullptr, SkinningJob::kLinear);
  CHECK(linear.Run());

  for (int v = 0; v < vertex_count; ++v) {
    const float* p = &mesh.positions[v * 3];
    ref::Result expected = ref::Skin(matrices.data(), nullptr,
                                     &mesh.indices[v * 2], 2, &mesh.weights[v],
                                     p, &mesh.normals[v * 3],
                                     &mesh.tangents[v * 3]);
    CHECK(expected.dual);
    CheckVec3(&dq_pos[v * 3], expected.position, 1e-4f, "twist position");
    CheckVec3(&dq_nrm[v * 3], expected.normal, 1e-4f, "twist normal");
    CheckVec3(&dq_tan[v * 3], expected.tangent, 1e-4f, "twist tangent");

    // The radius around the twist axis (x) must be preserved.
    const float radius =
        std::sqrt(dq_pos[v * 3 + 1] * dq_pos[v * 3 + 1] +
                  dq_pos[v * 3 + 2] * dq_pos[v * 3 + 2]);
    CheckClose(radius, 0.6f, 1e-4f, "dual quaternion radius");

    // Linear blend skinning squashes the cross-section.
    const float linear_radius =
        std::sqrt(lin_pos[v * 3 + 1] * lin_pos[v * 3 + 1] +
                  lin_pos[v * 3 + 2] * lin_pos[v * 3 + 2]);
    CHECK(linear_radius < 0.35f * 0.6f);

    // Normals and tangents are rotated without translation and keep their
    // length.
    const float normal_length = std::sqrt(
        dq_nrm[v * 3 + 0] * dq_nrm[v * 3 + 0] +
        dq_nrm[v * 3 + 1] * dq_nrm[v * 3 + 1] +
        dq_nrm[v * 3 + 2] * dq_nrm[v * 3 + 2]);
    CheckClose(normal_length, 1.f, 1e-4f, "normal length");
    const float tangent_length = std::sqrt(
        dq_tan[v * 3 + 0] * dq_tan[v * 3 + 0] +
        dq_tan[v * 3 + 1] * dq_tan[v * 3 + 1] +
        dq_tan[v * 3 + 2] * dq_tan[v * 3 + 2]);
    CheckClose(tangent_length, 1.f, 1e-4f, "tangent length");
  }
}

// Influences with translations and different rotation axes, including an
// orthogonal quaternion pair (dot product exactly zero, no sign flip).
void TestTranslationAndAxes() {
  std::vector<ozz::math::Float4x4> matrices{
      RigidX(0.7f, 1.f, 2.f, 3.f), RigidY(-1.1f, -2.f, 0.f, 4.f),
      RigidZ(2.0f, 0.5f, -1.f, 0.25f)};

  const int vertex_count = 6;
  SimpleMesh mesh;
  mesh.Resize(vertex_count, 3, true, true);
  for (int v = 0; v < vertex_count; ++v) {
    mesh.indices[v * 3 + 0] = 0;
    mesh.indices[v * 3 + 1] = 1;
    mesh.indices[v * 3 + 2] = 2;
    mesh.weights[v * 2 + 0] = .4f;
    mesh.weights[v * 2 + 1] = .3f;
    mesh.positions[v * 3 + 0] = 0.1f + 0.2f * v;
    mesh.positions[v * 3 + 1] = -0.15f * v;
    mesh.positions[v * 3 + 2] = 0.05f * v;
    mesh.normals[v * 3 + 0] = 0.1f;
    mesh.normals[v * 3 + 1] = 0.2f;
    mesh.normals[v * 3 + 2] = 0.3f;
    mesh.tangents[v * 3 + 0] = 0.03f;
    mesh.tangents[v * 3 + 1] = 0.04f;
    mesh.tangents[v * 3 + 2] = 0.05f;
  }
  std::vector<float> pos(mesh.positions.size()), nrm(mesh.normals.size()),
      tan(mesh.tangents.size());
  SkinningJob job = MakeJob(mesh, matrices, nullptr, pos.data(), nrm.data(),
                            tan.data(), SkinningJob::kDualQuaternion);
  CHECK(job.Run());
  for (int v = 0; v < vertex_count; ++v) {
    ref::Result expected =
        ref::Skin(matrices.data(), nullptr, &mesh.indices[v * 3], 3,
                  &mesh.weights[v * 2], &mesh.positions[v * 3],
                  &mesh.normals[v * 3], &mesh.tangents[v * 3]);
    CHECK(expected.dual);
    CheckVec3(&pos[v * 3], expected.position, 1e-4f, "axes position");
    CheckVec3(&nrm[v * 3], expected.normal, 1e-4f, "axes normal");
    CheckVec3(&tan[v * 3], expected.tangent, 1e-4f, "axes tangent");
  }

  // Orthogonal quaternion pair: identity and a 180 degrees rotation around x.
  // The dot product with the reference is exactly 0, so no influence is flipped.
  std::vector<ozz::math::Float4x4> orthogonal{Identity(),
                                              RigidX(3.14159265358979323846f,
                                                     0.f, 0.f, 0.f)};
  uint16_t orthogonal_indices[2] = {0, 1};
  float orthogonal_weights[1] = {.5f};
  float orthogonal_position[3] = {0.f, 1.f, 0.f};
  std::vector<float> orthogonal_out(3);
  SkinningJob orthogonal_job;
  orthogonal_job.vertex_count = 1;
  orthogonal_job.mode = SkinningJob::kDualQuaternion;
  orthogonal_job.influences_count = 2;
  orthogonal_job.joint_matrices = {orthogonal.data(), orthogonal.size()};
  orthogonal_job.joint_indices = {orthogonal_indices, 2};
  orthogonal_job.joint_indices_stride = sizeof(uint16_t) * 2;
  orthogonal_job.joint_weights = {orthogonal_weights, 1};
  orthogonal_job.joint_weights_stride = sizeof(float);
  orthogonal_job.in_positions = {orthogonal_position, 3};
  orthogonal_job.in_positions_stride = sizeof(float) * 3;
  orthogonal_job.out_positions = {orthogonal_out.data(), 3};
  orthogonal_job.out_positions_stride = sizeof(float) * 3;
  CHECK(orthogonal_job.Run());
  ref::Result expected = ref::Skin(orthogonal.data(), nullptr,
                                   orthogonal_indices, 2, orthogonal_weights,
                                   orthogonal_position, nullptr, nullptr);
  CHECK(expected.dual);
  CheckVec3(orthogonal_out.data(), expected.position, 1e-4f,
            "orthogonal position");
}

// Same mesh, same palette: rigid vertices go through dual quaternion skinning,
// while scaled, sheared and reflected vertices take the linear fallback.
void TestMixedRigidAndNonRigid() {
  std::vector<ozz::math::Float4x4> matrices{
      RigidX(0.9f, 0.5f, -1.f, 2.f),
      RigidY(-1.4f, 0.f, 1.f, 0.f),
      Scaling(1.f, 2.f, 3.f),                              // non-uniform scale
      Scaling(2.f, 2.f, 2.f),                              // uniform scale
      MakeMatrix(-1.f, 0.f, 0.f, 0.f, 0.f, 1.f, 0.f, 0.f,  // reflection
                 0.f, 0.f, 1.f, 0.f, 4.f, 0.f, 0.f, 1.f),
      MakeMatrix(1.f, 0.f, 0.f, 0.f, 0.5f, 1.f, 0.f, 0.f,  // shear
                 0.f, 0.f, 1.f, 0.f, 0.f, 0.f, 0.f, 1.f)};

  std::vector<ozz::math::Float4x4> it_matrices;
  for (const auto& m : matrices) {
    it_matrices.push_back(
        ozz::math::Transpose(ozz::math::Invert(m, nullptr)));
  }

  const int vertex_count = 6;
  SimpleMesh mesh;
  mesh.Resize(vertex_count, 2, true, true);
  // v0: two rigid influences. v1: non-uniform scale. v2: uniform scale.
  // v3: reflection. v4: shear. v5: one rigid influence.
  const int kIndexPairs[vertex_count][2] = {
      {0, 1}, {2, 2}, {3, 3}, {4, 4}, {5, 5}, {0, 0}};
  for (int v = 0; v < vertex_count; ++v) {
    mesh.indices[v * 2 + 0] = static_cast<uint16_t>(kIndexPairs[v][0]);
    mesh.indices[v * 2 + 1] = static_cast<uint16_t>(kIndexPairs[v][1]);
    mesh.weights[v] = .5f;
    mesh.positions[v * 3 + 0] = 0.2f + 0.3f * v;
    mesh.positions[v * 3 + 1] = 0.4f - 0.1f * v;
    mesh.positions[v * 3 + 2] = -0.2f + 0.05f * v;
    mesh.normals[v * 3 + 0] = 0.2f;
    mesh.normals[v * 3 + 1] = 0.5f;
    mesh.normals[v * 3 + 2] = -0.1f;
    mesh.tangents[v * 3 + 0] = 0.3f;
    mesh.tangents[v * 3 + 1] = -0.2f;
    mesh.tangents[v * 3 + 2] = 0.4f;
  }

  std::vector<float> dq_pos(mesh.positions.size()), dq_nrm(mesh.normals.size()),
      dq_tan(mesh.tangents.size());
  std::vector<float> lin_pos(mesh.positions.size()), lin_nrm(mesh.normals.size()),
      lin_tan(mesh.tangents.size());
  SkinningJob dq = MakeJob(mesh, matrices, &it_matrices, dq_pos.data(),
                           dq_nrm.data(), dq_tan.data(),
                           SkinningJob::kDualQuaternion);
  CHECK(dq.Run());
  SkinningJob linear = MakeJob(mesh, matrices, &it_matrices, lin_pos.data(),
                               lin_nrm.data(), lin_tan.data(),
                               SkinningJob::kLinear);
  CHECK(linear.Run());

  for (int v = 0; v < vertex_count; ++v) {
    ref::Result expected =
        ref::Skin(matrices.data(), it_matrices.data(), &mesh.indices[v * 2], 2,
                  &mesh.weights[v], &mesh.positions[v * 3],
                  &mesh.normals[v * 3], &mesh.tangents[v * 3]);
    if (expected.dual) {
      // Rigid vertex: dual quaternion, and different from linear blend.
      CHECK(v == 0 || v == 5);
      CheckVec3(&dq_pos[v * 3], expected.position, 1e-4f, "mixed dq position");
      CheckVec3(&dq_nrm[v * 3], expected.normal, 1e-4f, "mixed dq normal");
      CheckVec3(&dq_tan[v * 3], expected.tangent, 1e-4f, "mixed dq tangent");
    } else {
      // Non rigid vertex: must exactly match the legacy linear result.
      CHECK(v >= 1 && v <= 4);
      CheckVec3(&dq_pos[v * 3], &lin_pos[v * 3], 1e-5f, "fallback position");
      CheckVec3(&dq_nrm[v * 3], &lin_nrm[v * 3], 1e-5f, "fallback normal");
      CheckVec3(&dq_tan[v * 3], &lin_tan[v * 3], 1e-5f, "fallback tangent");
    }
  }
}

// A zero-weight non-rigid influence must not trigger the fallback, even when
// its matrix contains non-finite values. Zero-weight matrices are still
// referenced through a valid palette index.
void TestZeroWeightNonRigid() {
  // Index 1 holds a non-finite, non-rigid matrix that is only zero-weighted.
  std::vector<ozz::math::Float4x4> matrices{RigidY(1.2f, 0.3f, 2.f, -0.5f)};
  ozz::math::Float4x4 nan_matrix = Identity();
  nan_matrix.cols[0] =
      ozz::math::simd_float4::Load(std::numeric_limits<float>::quiet_NaN(), 0.f,
                                   0.f, 0.f);
  nan_matrix.cols[1] =
      ozz::math::simd_float4::Load(0.f, 2.f, 0.f, 0.f);
  matrices.push_back(nan_matrix);

  const int vertex_count = 3;
  SimpleMesh mesh;
  mesh.Resize(vertex_count, 2, true, true);
  for (int v = 0; v < vertex_count; ++v) {
    mesh.indices[v * 2 + 0] = 0;  // rigid influence, weight 1
    mesh.indices[v * 2 + 1] = 1;  // non-rigid influence, weight 0
    mesh.weights[v] = 1.f;
    mesh.positions[v * 3 + 0] = 0.5f - 0.2f * v;
    mesh.positions[v * 3 + 1] = 1.f + 0.1f * v;
    mesh.positions[v * 3 + 2] = -1.f;
    mesh.normals[v * 3 + 0] = 0.f;
    mesh.normals[v * 3 + 1] = 1.f;
    mesh.normals[v * 3 + 2] = 0.f;
    mesh.tangents[v * 3 + 0] = 1.f;
    mesh.tangents[v * 3 + 1] = 0.f;
    mesh.tangents[v * 3 + 2] = 0.f;
  }
  std::vector<float> pos(mesh.positions.size()), nrm(mesh.normals.size()),
      tan(mesh.tangents.size());
  SkinningJob job = MakeJob(mesh, matrices, nullptr, pos.data(), nrm.data(),
                            tan.data(), SkinningJob::kDualQuaternion);
  CHECK(job.Run());
  for (int v = 0; v < vertex_count; ++v) {
    ref::Result expected = ref::Skin(
        matrices.data(), nullptr, &mesh.indices[v * 2], 2, &mesh.weights[v],
        &mesh.positions[v * 3], &mesh.normals[v * 3], &mesh.tangents[v * 3]);
    CHECK(expected.dual);
    CHECK(std::isfinite(pos[v * 3 + 0]) && std::isfinite(pos[v * 3 + 1]) &&
          std::isfinite(pos[v * 3 + 2]));
    CheckVec3(&pos[v * 3], expected.position, 1e-4f, "zero weight position");
    CheckVec3(&nrm[v * 3], expected.normal, 1e-4f, "zero weight normal");
    CheckVec3(&tan[v * 3], expected.tangent, 1e-4f, "zero weight tangent");
  }
}

// More than four influences, both on the dual quaternion path and on the
// linear fallback path. Also covers the dedicated 4 influences case.
void TestManyInfluences() {
  std::vector<ozz::math::Float4x4> rigid{
      RigidX(0.6f, 1.f, 0.f, 0.f),     RigidY(-0.9f, 0.f, 1.f, 0.f),
      RigidZ(1.3f, 0.f, 0.f, 1.f),     Rigid(0.57735f, 0.57735f, 0.57735f,
                                             0.4f, -1.f, 0.f, 2.f),
      RigidX(-1.7f, 2.f, 1.f, 0.f),    RigidY(2.1f, 0.f, 0.f, -1.f)};

  // Four influences, rigid palette: dual quaternion.
  {
    const int vertex_count = 3;
    SimpleMesh mesh;
    mesh.Resize(vertex_count, 4, true, true);
    const float four_weights[3] = {.3f, .25f, .2f};  // sum .75
    for (int v = 0; v < vertex_count; ++v) {
      for (int j = 0; j < 4; ++j) {
        mesh.indices[v * 4 + j] = static_cast<uint16_t>((j + v) % 4);
      }
      for (int j = 0; j < 3; ++j) {
        mesh.weights[v * 3 + j] = four_weights[j];
      }
      mesh.positions[v * 3 + 0] = 0.2f * v;
      mesh.positions[v * 3 + 1] = 0.4f;
      mesh.positions[v * 3 + 2] = -0.1f;
      mesh.normals[v * 3 + 0] = 0.f;
      mesh.normals[v * 3 + 1] = 1.f;
      mesh.normals[v * 3 + 2] = 0.f;
      mesh.tangents[v * 3 + 0] = 1.f;
      mesh.tangents[v * 3 + 1] = 0.f;
      mesh.tangents[v * 3 + 2] = 0.f;
    }
    std::vector<float> pos(mesh.positions.size()), nrm(mesh.normals.size()),
        tan(mesh.tangents.size());
    SkinningJob job = MakeJob(mesh, rigid, nullptr, pos.data(), nrm.data(),
                              tan.data(), SkinningJob::kDualQuaternion);
    CHECK(job.Run());
    for (int v = 0; v < vertex_count; ++v) {
      ref::Result expected =
          ref::Skin(rigid.data(), nullptr, &mesh.indices[v * 4], 4,
                    &mesh.weights[v * 3], &mesh.positions[v * 3],
                    &mesh.normals[v * 3], &mesh.tangents[v * 3]);
      CHECK(expected.dual);
      CheckVec3(&pos[v * 3], expected.position, 1e-4f, "four influences");
      CheckVec3(&nrm[v * 3], expected.normal, 1e-4f, "four influences normal");
      CheckVec3(&tan[v * 3], expected.tangent, 1e-4f, "four influences tangent");
    }
  }
  const int vertex_count = 4;
  SimpleMesh mesh;
  mesh.Resize(vertex_count, 6, true, true);
  const float explicit_weights[5] = {.1f, .2f, .15f, .05f, .3f};  // sum .8
  for (int v = 0; v < vertex_count; ++v) {
    for (int j = 0; j < 6; ++j) {
      mesh.indices[v * 6 + j] = static_cast<uint16_t>((j + v) % 6);
    }
    for (int j = 0; j < 5; ++j) {
      mesh.weights[v * 5 + j] = explicit_weights[j];
    }
    mesh.positions[v * 3 + 0] = 0.1f * v;
    mesh.positions[v * 3 + 1] = 0.7f;
    mesh.positions[v * 3 + 2] = -0.4f + 0.1f * v;
    mesh.normals[v * 3 + 0] = 0.f;
    mesh.normals[v * 3 + 1] = 0.6f;
    mesh.normals[v * 3 + 2] = 0.8f;
    mesh.tangents[v * 3 + 0] = 0.8f;
    mesh.tangents[v * 3 + 1] = -0.6f;
    mesh.tangents[v * 3 + 2] = 0.f;
  }
  std::vector<float> pos(mesh.positions.size()), nrm(mesh.normals.size()),
      tan(mesh.tangents.size());
  SkinningJob job = MakeJob(mesh, rigid, nullptr, pos.data(), nrm.data(),
                            tan.data(), SkinningJob::kDualQuaternion);
  CHECK(job.Run());
  for (int v = 0; v < vertex_count; ++v) {
    ref::Result expected =
        ref::Skin(rigid.data(), nullptr, &mesh.indices[v * 6], 6,
                  &mesh.weights[v * 5], &mesh.positions[v * 3],
                  &mesh.normals[v * 3], &mesh.tangents[v * 3]);
    CHECK(expected.dual);
    CheckVec3(&pos[v * 3], expected.position, 1e-4f, "many influences position");
    CheckVec3(&nrm[v * 3], expected.normal, 1e-4f, "many influences normal");
    CheckVec3(&tan[v * 3], expected.tangent, 1e-4f, "many influences tangent");
  }

  // Same number of influences, but one positively weighted matrix is a scale:
  // the whole vertex must match the legacy linear mode.
  std::vector<ozz::math::Float4x4> mixed = rigid;
  mixed[3] = Scaling(1.f, 3.f, 0.5f);
  std::vector<ozz::math::Float4x4> it_matrices;
  for (const auto& m : mixed) {
    it_matrices.push_back(ozz::math::Transpose(ozz::math::Invert(m)));
  }
  std::vector<float> dq_pos(mesh.positions.size()), dq_nrm(mesh.normals.size()),
      dq_tan(mesh.tangents.size());
  std::vector<float> lin_pos(mesh.positions.size()), lin_nrm(mesh.normals.size()),
      lin_tan(mesh.tangents.size());
  SkinningJob dq = MakeJob(mesh, mixed, &it_matrices, dq_pos.data(),
                           dq_nrm.data(), dq_tan.data(),
                           SkinningJob::kDualQuaternion);
  CHECK(dq.Run());
  SkinningJob linear = MakeJob(mesh, mixed, &it_matrices, lin_pos.data(),
                               lin_nrm.data(), lin_tan.data(),
                               SkinningJob::kLinear);
  CHECK(linear.Run());
  for (int v = 0; v < vertex_count; ++v) {
    ref::Result expected =
        ref::Skin(mixed.data(), it_matrices.data(), &mesh.indices[v * 6], 6,
                  &mesh.weights[v * 5], &mesh.positions[v * 3],
                  &mesh.normals[v * 3], &mesh.tangents[v * 3]);
    CHECK(!expected.dual);
    CheckVec3(&dq_pos[v * 3], &lin_pos[v * 3], 1e-5f, "many fallback position");
    CheckVec3(&dq_nrm[v * 3], &lin_nrm[v * 3], 1e-5f, "many fallback normal");
    CheckVec3(&dq_tan[v * 3], &lin_tan[v * 3], 1e-5f, "many fallback tangent");
  }
}

// Interleaved attributes, in-place (fully aliased) input/output, preserved
// color, uv, tangent fourth component and padding.
struct InterleavedVertex {
  float color[4];
  float uv[2];
  float position[3];
  float normal[3];
  float tangent[4];
  uint8_t padding[12];
};

void TestInterleavedInPlace() {
  std::vector<ozz::math::Float4x4> matrices{RigidZ(1.1f, 1.f, -1.f, 0.5f),
                                            RigidY(-0.8f, 0.f, 2.f, 1.f)};
  const int vertex_count = 5;
  std::vector<InterleavedVertex> vertices(vertex_count);
  std::vector<InterleavedVertex> reference(vertex_count);
  std::vector<uint16_t> indices(vertex_count * 2);
  std::vector<float> weights(vertex_count);
  for (int v = 0; v < vertex_count; ++v) {
    InterleavedVertex& vertex = vertices[v];
    vertex.color[0] = 0.11f * v;
    vertex.color[1] = 0.22f * v;
    vertex.color[2] = 0.33f * v;
    vertex.color[3] = 0.44f * v;
    vertex.uv[0] = 0.5f + 0.01f * v;
    vertex.uv[1] = 0.25f + 0.02f * v;
    vertex.position[0] = 0.6f + 0.1f * v;
    vertex.position[1] = -0.2f * v;
    vertex.position[2] = 0.3f;
    vertex.normal[0] = 0.f;
    vertex.normal[1] = 0.f;
    vertex.normal[2] = 1.f;
    vertex.tangent[0] = 1.f;
    vertex.tangent[1] = 0.f;
    vertex.tangent[2] = 0.f;
    vertex.tangent[3] = -1.f;  // handedness, must be preserved
    for (int p = 0; p < 12; ++p) {
      vertex.padding[p] = static_cast<uint8_t>(0x80 + v + p);
    }
    indices[v * 2 + 0] = static_cast<uint16_t>(v % 2);
    indices[v * 2 + 1] = static_cast<uint16_t>((v + 1) % 2);
    weights[v] = .6f;
  }
  reference = vertices;

  SkinningJob job;
  job.vertex_count = vertex_count;
  job.mode = SkinningJob::kDualQuaternion;
  job.influences_count = 2;
  job.joint_matrices = {matrices.data(), matrices.size()};
  job.joint_indices = {indices.data(), indices.size()};
  job.joint_indices_stride = sizeof(uint16_t) * 2;
  job.joint_weights = {weights.data(), weights.size()};
  job.joint_weights_stride = sizeof(float);
  // Interleaved spans are expressed in elements, not bytes. Each attribute
  // span covers the last vertex's three float components.
  const size_t float_span =
      (sizeof(InterleavedVertex) * (vertex_count - 1) + sizeof(float) * 3) /
      sizeof(float);
  job.in_positions = {vertices.data()->position, float_span};
  job.in_positions_stride = sizeof(InterleavedVertex);
  job.out_positions = {vertices.data()->position, float_span};
  job.out_positions_stride = sizeof(InterleavedVertex);
  job.in_normals = {vertices.data()->normal, float_span};
  job.in_normals_stride = sizeof(InterleavedVertex);
  job.out_normals = {vertices.data()->normal, float_span};
  job.out_normals_stride = sizeof(InterleavedVertex);
  job.in_tangents = {vertices.data()->tangent, float_span};
  job.in_tangents_stride = sizeof(InterleavedVertex);
  job.out_tangents = {vertices.data()->tangent, float_span};
  job.out_tangents_stride = sizeof(InterleavedVertex);
  CHECK(job.Run());

  for (int v = 0; v < vertex_count; ++v) {
    ref::Result expected = ref::Skin(
        matrices.data(), nullptr, &indices[v * 2], 2, &weights[v],
        reference[v].position, reference[v].normal, reference[v].tangent);
    CHECK(expected.dual);
    CheckVec3(vertices[v].position, expected.position, 1e-4f,
              "interleaved position");
    CheckVec3(vertices[v].normal, expected.normal, 1e-4f, "interleaved normal");
    CheckVec3(vertices[v].tangent, expected.tangent, 1e-4f,
              "interleaved tangent");
    // Untouched attributes.
    CHECK(std::memcmp(vertices[v].color, reference[v].color, sizeof(float) * 4) ==
          0);
    CHECK(std::memcmp(vertices[v].uv, reference[v].uv, sizeof(float) * 2) == 0);
    CheckClose(vertices[v].tangent[3], -1.f, 0.f, "tangent w preserved");
    CHECK(std::memcmp(vertices[v].padding, reference[v].padding, 12) == 0);
  }
}

// Invalid input on the last vertex must leave every output untouched.
void TestInvalidLastVertex() {
  std::vector<ozz::math::Float4x4> matrices{RigidX(0.4f, 0.f, 0.f, 0.f),
                                            RigidY(0.7f, 0.f, 0.f, 0.f)};
  const int vertex_count = 3;
  SimpleMesh mesh;
  mesh.Resize(vertex_count, 2, true, true);
  for (int v = 0; v < vertex_count; ++v) {
    mesh.indices[v * 2 + 0] = 0;
    mesh.indices[v * 2 + 1] = 1;
    mesh.weights[v] = .5f;
    mesh.positions[v * 3 + 0] = 1.f + v;
    mesh.positions[v * 3 + 1] = 2.f;
    mesh.positions[v * 3 + 2] = 3.f;
    mesh.normals[v * 3 + 0] = 0.f;
    mesh.normals[v * 3 + 1] = 1.f;
    mesh.normals[v * 3 + 2] = 0.f;
    mesh.tangents[v * 3 + 0] = 1.f;
    mesh.tangents[v * 3 + 1] = 0.f;
    mesh.tangents[v * 3 + 2] = 0.f;
  }

  const float sentinel = 12345.678f;
  auto run_case = [&](const char* _label) {
    std::vector<float> pos(mesh.positions.size(), sentinel);
    std::vector<float> nrm(mesh.normals.size(), sentinel);
    std::vector<float> tan(mesh.tangents.size(), sentinel);
    SkinningJob job = MakeJob(mesh, matrices, nullptr, pos.data(), nrm.data(),
                              tan.data(), SkinningJob::kDualQuaternion);
    CHECK(!job.Validate());
    CHECK(!job.Run());
    for (size_t i = 0; i < pos.size(); ++i) {
      CheckClose(pos[i], sentinel, 0.f, _label);
    }
    for (size_t i = 0; i < nrm.size(); ++i) {
      CheckClose(nrm[i], sentinel, 0.f, _label);
    }
    for (size_t i = 0; i < tan.size(); ++i) {
      CheckClose(tan[i], sentinel, 0.f, _label);
    }
  };

  // A palette index out of range, on the last vertex.
  mesh.indices[(vertex_count - 1) * 2 + 1] = 7;
  run_case("out of range index");
  mesh.indices[(vertex_count - 1) * 2 + 1] = 1;

  // A zero-weight influence with an out of range index must still be rejected.
  mesh.weights[vertex_count - 1] = 1.f;
  mesh.indices[(vertex_count - 1) * 2 + 1] = 9;
  run_case("out of range zero weight index");
  mesh.indices[(vertex_count - 1) * 2 + 1] = 1;

  // An explicit weight greater than 1.
  mesh.weights[vertex_count - 1] = 1.5f;
  run_case("weight greater than 1");
  mesh.weights[vertex_count - 1] = .5f;

  // A non finite explicit weight.
  mesh.weights[vertex_count - 1] =
      std::numeric_limits<float>::quiet_NaN();
  run_case("non finite weight");
  mesh.weights[vertex_count - 1] = .5f;

  // A negative explicit weight.
  mesh.weights[vertex_count - 1] = -0.25f;
  run_case("negative weight");
  mesh.weights[vertex_count - 1] = .5f;

  // An explicit weight sum greater than 1 (the restored weight would be
  // negative).
  {
    SimpleMesh mesh3;
    mesh3.Resize(vertex_count, 3, true, true);
    for (int v = 0; v < vertex_count; ++v) {
      mesh3.indices[v * 3 + 0] = 0;
      mesh3.indices[v * 3 + 1] = 1;
      mesh3.indices[v * 3 + 2] = 0;
      mesh3.weights[v * 2 + 0] = .5f;
      mesh3.weights[v * 2 + 1] = .5f;
    }
    mesh3.weights[(vertex_count - 1) * 2 + 1] = .6f;
    std::vector<float> pos(mesh3.positions.size(), sentinel);
    std::vector<float> nrm(mesh3.normals.size(), sentinel);
    std::vector<float> tan(mesh3.tangents.size(), sentinel);
    SkinningJob job = MakeJob(mesh3, matrices, nullptr, pos.data(), nrm.data(),
                              tan.data(), SkinningJob::kDualQuaternion);
    CHECK(!job.Validate());
    CHECK(!job.Run());
    for (float value : pos) CheckClose(value, sentinel, 0.f, "weight sum");
    for (float value : nrm) CheckClose(value, sentinel, 0.f, "weight sum");
    for (float value : tan) CheckClose(value, sentinel, 0.f, "weight sum");
  }

  // The legacy linear mode keeps accepting this data, as it doesn't perform
  // any of the extra data checks.
  {
    std::vector<float> pos(mesh.positions.size(), sentinel);
    mesh.weights[0] = .75f;
    mesh.weights[1] = .75f;
    mesh.weights[2] = .75f;
    SkinningJob linear = MakeJob(mesh, matrices, nullptr, pos.data(), nullptr,
                                 nullptr, SkinningJob::kLinear);
    CHECK(linear.Validate());
    CHECK(linear.Run());
    // And a non-finite weight is accepted by the legacy linear mode.
    mesh.weights[0] = std::numeric_limits<float>::quiet_NaN();
    SkinningJob nan_job = MakeJob(mesh, matrices, nullptr, pos.data(), nullptr,
                                  nullptr, SkinningJob::kLinear);
    CHECK(nan_job.Validate());
    CHECK(nan_job.Run());
    SkinningJob nan_dq = MakeJob(mesh, matrices, nullptr, pos.data(), nullptr,
                                 nullptr, SkinningJob::kDualQuaternion);
    CHECK(!nan_dq.Validate());
    CHECK(!nan_dq.Run());
  }
}

// A single rigid influence is equivalent to the matrix transform itself.
void TestSingleRigidInfluence() {
  std::vector<ozz::math::Float4x4> matrices{
      Rigid(0.3f, -0.4f, 0.86602540378f, 2.5f, 1.5f, -2.f, 0.25f),
      RigidX(3.14159265358979323846f, 0.f, 0.f, 0.f)};
  for (size_t m = 0; m < matrices.size(); ++m) {
    const int vertex_count = 3;
    SimpleMesh mesh;
    mesh.Resize(vertex_count, 1, true, true);
    for (int v = 0; v < vertex_count; ++v) {
      mesh.indices[v] = static_cast<uint16_t>(m);
      mesh.positions[v * 3 + 0] = 0.f;
      mesh.positions[v * 3 + 1] = 0.5f + v;
      mesh.positions[v * 3 + 2] = 1.f - v;
      mesh.normals[v * 3 + 0] = 0.6f;
      mesh.normals[v * 3 + 1] = 0.8f;
      mesh.normals[v * 3 + 2] = 0.f;
      mesh.tangents[v * 3 + 0] = 0.f;
      mesh.tangents[v * 3 + 1] = 0.f;
      mesh.tangents[v * 3 + 2] = 1.f;
    }
    std::vector<float> pos(mesh.positions.size()), nrm(mesh.normals.size()),
        tan(mesh.tangents.size());
    SkinningJob job = MakeJob(mesh, matrices, nullptr, pos.data(), nrm.data(),
                              tan.data(), SkinningJob::kDualQuaternion);
    CHECK(job.Run());
    for (int v = 0; v < vertex_count; ++v) {
      ref::Result expected =
          ref::Skin(matrices.data(), nullptr, &mesh.indices[v], 1, nullptr,
                    &mesh.positions[v * 3], &mesh.normals[v * 3],
                    &mesh.tangents[v * 3]);
      CHECK(expected.dual);
      CheckVec3(&pos[v * 3], expected.position, 1e-4f, "single position");
      CheckVec3(&nrm[v * 3], expected.normal, 1e-4f, "single normal");
      CheckVec3(&tan[v * 3], expected.tangent, 1e-4f, "single tangent");
    }
  }
}

// Compares the dual quaternion rigid transform against a direct affine
// application of each reference matrix to the vertex, for the specific case of
// two influences: the blended dual quaternion must be a proper rigid transform
// (orthonormal matrix with +1 determinant) and must preserve pairwise distances.
// This check doesn't use the reference quaternion arithmetic; it works directly
// from the resulting positions and the input matrices.
void TestRigidityAgainstMatrices() {
  std::vector<ozz::math::Float4x4> matrices{
      RigidX(1.9f, 0.3f, -0.2f, 1.5f), RigidY(-2.4f, -1.f, 0.5f, 0.25f)};

  const int vertex_count = 12;
  SimpleMesh mesh;
  mesh.Resize(vertex_count, 2, false, false);
  for (int v = 0; v < vertex_count; ++v) {
    const float angle = static_cast<float>(v) * 0.5f;
    mesh.indices[v * 2 + 0] = 0;
    mesh.indices[v * 2 + 1] = 1;
    mesh.weights[v] = .35f;
    mesh.positions[v * 3 + 0] = std::cos(angle);
    mesh.positions[v * 3 + 1] = std::sin(angle) * 0.5f;
    mesh.positions[v * 3 + 2] = 0.25f * static_cast<float>(v % 3);
  }
  std::vector<float> pos(mesh.positions.size());
  SkinningJob job = MakeJob(mesh, matrices, nullptr, pos.data(), nullptr,
                            nullptr, SkinningJob::kDualQuaternion);
  CHECK(job.Run());

  // Pairwise distance preservation, directly from the output positions.
  for (int a = 0; a < vertex_count; ++a) {
    for (int b = a + 1; b < vertex_count; ++b) {
      float in_delta[3], out_delta[3];
      for (int i = 0; i < 3; ++i) {
        in_delta[i] = mesh.positions[a * 3 + i] - mesh.positions[b * 3 + i];
        out_delta[i] = pos[a * 3 + i] - pos[b * 3 + i];
      }
      const float in_length = std::sqrt(in_delta[0] * in_delta[0] +
                                        in_delta[1] * in_delta[1] +
                                        in_delta[2] * in_delta[2]);
      const float out_length = std::sqrt(out_delta[0] * out_delta[0] +
                                         out_delta[1] * out_delta[1] +
                                         out_delta[2] * out_delta[2]);
      CheckClose(out_length, in_length, 1e-4f, "rigidity distance");
    }
  }

  // The blended transform is rigid: it maps the origin and the three canonical
  // basis vectors. Reconstruct the 3x3 linear part from four input points, then
  // check its orthonormality and +1 determinant.
  {
    const int corner_count = 4;
    SimpleMesh corners;
    corners.Resize(corner_count, 2, false, false);
    const float origins[corner_count][3] = {
        {0.f, 0.f, 0.f}, {1.f, 0.f, 0.f}, {0.f, 1.f, 0.f}, {0.f, 0.f, 1.f}};
    for (int c = 0; c < corner_count; ++c) {
      corners.indices[c * 2 + 0] = 0;
      corners.indices[c * 2 + 1] = 1;
      corners.weights[c] = .35f;
      for (int i = 0; i < 3; ++i) {
        corners.positions[c * 3 + i] = origins[c][i];
      }
    }
    std::vector<float> out(corners.positions.size());
    SkinningJob corner_job = MakeJob(corners, matrices, nullptr, out.data(),
                                     nullptr, nullptr,
                                     SkinningJob::kDualQuaternion);
    CHECK(corner_job.Run());
    float linear[3][3];
    for (int axis = 0; axis < 3; ++axis) {
      for (int i = 0; i < 3; ++i) {
        linear[i][axis] = out[(axis + 1) * 3 + i] - out[i];
      }
    }
    for (int c = 0; c < 3; ++c) {
      const float length = std::sqrt(linear[0][c] * linear[0][c] +
                                     linear[1][c] * linear[1][c] +
                                     linear[2][c] * linear[2][c]);
      CheckClose(length, 1.f, 1e-4f, "rigid column length");
    }
    for (int a = 0; a < 3; ++a) {
      for (int b = a + 1; b < 3; ++b) {
        const float dot = linear[0][a] * linear[0][b] +
                          linear[1][a] * linear[1][b] +
                          linear[2][a] * linear[2][b];
        CheckClose(dot, 0.f, 1e-4f, "rigid column dot");
      }
    }
    const float determinant =
        linear[0][0] * (linear[1][1] * linear[2][2] -
                        linear[1][2] * linear[2][1]) -
        linear[0][1] * (linear[1][0] * linear[2][2] -
                        linear[1][2] * linear[2][0]) +
        linear[0][2] * (linear[1][0] * linear[2][1] -
                        linear[1][1] * linear[2][0]);
    CheckClose(determinant, 1.f, 1e-4f, "rigid determinant");
  }

  // The single influence result must equal the matrix transform exactly.
  {
    uint16_t single_index[1] = {0};
    float single_position[3] = {0.4f, -0.7f, 1.3f};
    std::vector<float> single_out(3);
    SkinningJob single_job;
    single_job.vertex_count = 1;
    single_job.mode = SkinningJob::kDualQuaternion;
    single_job.influences_count = 1;
    single_job.joint_matrices = {matrices.data(), matrices.size()};
    single_job.joint_indices = {single_index, 1};
    single_job.joint_indices_stride = sizeof(uint16_t);
    single_job.in_positions = {single_position, 3};
    single_job.in_positions_stride = sizeof(float) * 3;
    single_job.out_positions = {single_out.data(), 3};
    single_job.out_positions_stride = sizeof(float) * 3;
    CHECK(single_job.Run());
    const ozz::math::SimdFloat4 p = ozz::math::simd_float4::Load3PtrU(
        single_position);
    const ozz::math::SimdFloat4 expected =
        ozz::math::TransformPoint(matrices[0], p);
    const float expected_position[3] = {ozz::math::GetX(expected),
                                        ozz::math::GetY(expected),
                                        ozz::math::GetZ(expected)};
    CheckVec3(single_out.data(), expected_position, 1e-4f,
              "single influence equals matrix");
  }
}

// Sign handling: the reference influence is the strictly positive weight with
// the greatest value, ties being broken by the smallest palette index. Its
// rotation sets the hemisphere every influence is aligned to.
void TestReferenceSelectionAndHemisphere() {
  // Matrices 0 and 1 are the same rotation expressed with opposite quaternion
  // signs (identical matrices, so the same rotation). Matrix 2 is a different
  // rotation whose quaternion has a negative w component before canonization.
  std::vector<ozz::math::Float4x4> matrices{
      RigidZ(0.6f, 1.f, 0.f, 0.f), RigidZ(0.6f, 1.f, 0.f, 0.f),
      RigidZ(-2.4f, 0.f, 1.f, 0.f)};

  // Vertex A: influences 1 and 0 have equal weights, the smallest palette index
  // (0) is the reference. Vertex B: influence 1 has the greatest weight.
  const int vertex_count = 8;
  SimpleMesh mesh;
  mesh.Resize(vertex_count, 2, true, true);
  for (int v = 0; v < vertex_count; ++v) {
    mesh.indices[v * 2 + 0] = static_cast<uint16_t>(v % 3);
    mesh.indices[v * 2 + 1] = static_cast<uint16_t>((v + 1) % 3);
    // For v = 0, indices are (0,1) with equal weights: reference is 0.
    mesh.weights[v] = v == 0 ? .5f : .25f + .05f * static_cast<float>(v);
    mesh.positions[v * 3 + 0] = -0.5f + 0.3f * v;
    mesh.positions[v * 3 + 1] = 0.9f - 0.1f * v;
    mesh.positions[v * 3 + 2] = 0.2f;
    mesh.normals[v * 3 + 0] = 0.5f;
    mesh.normals[v * 3 + 1] = 0.5f;
    mesh.normals[v * 3 + 2] = 0.70710678f;
    mesh.tangents[v * 3 + 0] = 0.70710678f;
    mesh.tangents[v * 3 + 1] = 0.70710678f;
    mesh.tangents[v * 3 + 2] = 0.f;
  }
  std::vector<float> pos(mesh.positions.size()), nrm(mesh.normals.size()),
      tan(mesh.tangents.size());
  SkinningJob job = MakeJob(mesh, matrices, nullptr, pos.data(), nrm.data(),
                            tan.data(), SkinningJob::kDualQuaternion);
  CHECK(job.Run());
  for (int v = 0; v < vertex_count; ++v) {
    ref::Result expected =
        ref::Skin(matrices.data(), nullptr, &mesh.indices[v * 2], 2,
                  &mesh.weights[v], &mesh.positions[v * 3],
                  &mesh.normals[v * 3], &mesh.tangents[v * 3]);
    CHECK(expected.dual);
    CheckVec3(&pos[v * 3], expected.position, 1e-4f, "hemisphere position");
    CheckVec3(&nrm[v * 3], expected.normal, 1e-4f, "hemisphere normal");
    CheckVec3(&tan[v * 3], expected.tangent, 1e-4f, "hemisphere tangent");
  }

  // A tie between equal weights on different palette entries must select the
  // smallest palette index, whichever order the influences are listed in.
  {
    std::vector<ozz::math::Float4x4> tie_matrices{RigidX(2.7f, 0.f, 0.f, 0.f),
                                                  RigidY(-0.4f, 1.f, 0.f, 0.f)};
    const uint16_t index_orders[2][2] = {{0, 1}, {1, 0}};
    float reference_position[3] = {0.3f, 0.6f, -0.9f};
    for (int order = 0; order < 2; ++order) {
      uint16_t indices[2] = {index_orders[order][0], index_orders[order][1]};
      float weights[1] = {.5f};
      std::vector<float> out(3);
      SkinningJob tie_job;
      tie_job.vertex_count = 1;
      tie_job.mode = SkinningJob::kDualQuaternion;
      tie_job.influences_count = 2;
      tie_job.joint_matrices = {tie_matrices.data(), tie_matrices.size()};
      tie_job.joint_indices = {indices, 2};
      tie_job.joint_indices_stride = sizeof(uint16_t) * 2;
      tie_job.joint_weights = {weights, 1};
      tie_job.joint_weights_stride = sizeof(float);
      tie_job.in_positions = {reference_position, 3};
      tie_job.in_positions_stride = sizeof(float) * 3;
      tie_job.out_positions = {out.data(), 3};
      tie_job.out_positions_stride = sizeof(float) * 3;
      CHECK(tie_job.Run());
      ref::Result expected = ref::Skin(tie_matrices.data(), nullptr, indices, 2,
                                       weights, reference_position, nullptr,
                                       nullptr);
      CHECK(expected.dual);
      CheckVec3(out.data(), expected.position, 1e-4f, "tie reference position");
    }
  }
}

// The restored last weight can be the greatest positive one, making the last
// influence the reference. Also checks a restored weight of exactly zero.
void TestRestoredWeightEdges() {
  std::vector<ozz::math::Float4x4> matrices{RigidX(0.5f, 1.f, 0.f, 0.f),
                                            RigidY(-0.7f, 0.f, 2.f, 0.f),
                                            RigidZ(1.4f, 0.f, 0.f, -1.f)};

  // Restored weight (1 - .1 - .1 = .8) is the greatest: the last influence is
  // the reference.
  {
    uint16_t indices[3] = {0, 1, 2};
    float weights[2] = {.1f, .1f};
    float position[3] = {0.3f, -0.2f, 0.9f};
    float normal[3] = {0.f, 1.f, 0.f};
    float tangent[3] = {1.f, 0.f, 0.f};
    std::vector<float> pos(3), nrm(3), tan(3);
    SkinningJob job;
    job.vertex_count = 1;
    job.mode = SkinningJob::kDualQuaternion;
    job.influences_count = 3;
    job.joint_matrices = {matrices.data(), matrices.size()};
    job.joint_indices = {indices, 3};
    job.joint_indices_stride = sizeof(uint16_t) * 3;
    job.joint_weights = {weights, 2};
    job.joint_weights_stride = sizeof(float) * 2;
    job.in_positions = {position, 3};
    job.in_positions_stride = sizeof(float) * 3;
    job.out_positions = {pos.data(), 3};
    job.out_positions_stride = sizeof(float) * 3;
    job.in_normals = {normal, 3};
    job.in_normals_stride = sizeof(float) * 3;
    job.out_normals = {nrm.data(), 3};
    job.out_normals_stride = sizeof(float) * 3;
    job.in_tangents = {tangent, 3};
    job.in_tangents_stride = sizeof(float) * 3;
    job.out_tangents = {tan.data(), 3};
    job.out_tangents_stride = sizeof(float) * 3;
    CHECK(job.Run());
    ref::Result expected =
        ref::Skin(matrices.data(), nullptr, indices, 3, weights, position,
                  normal, tangent);
    CHECK(expected.dual);
    CheckVec3(pos.data(), expected.position, 1e-4f, "restored reference");
    CheckVec3(nrm.data(), expected.normal, 1e-4f, "restored reference normal");
    CheckVec3(tan.data(), expected.tangent, 1e-4f, "restored reference tangent");
  }

  // Explicit weights sum to 1: the restored weight is exactly 0. The last
  // influence is a non-rigid (scale) matrix, which must not trigger a fallback.
  {
    std::vector<ozz::math::Float4x4> mixed = matrices;
    mixed[2] = Scaling(1.f, 2.f, 3.f);
    uint16_t indices[3] = {0, 1, 2};
    float weights[2] = {.6f, .4f};
    float position[3] = {-0.4f, 0.5f, 0.2f};
    std::vector<float> pos(3);
    SkinningJob job;
    job.vertex_count = 1;
    job.mode = SkinningJob::kDualQuaternion;
    job.influences_count = 3;
    job.joint_matrices = {mixed.data(), mixed.size()};
    job.joint_indices = {indices, 3};
    job.joint_indices_stride = sizeof(uint16_t) * 3;
    job.joint_weights = {weights, 2};
    job.joint_weights_stride = sizeof(float) * 2;
    job.in_positions = {position, 3};
    job.in_positions_stride = sizeof(float) * 3;
    job.out_positions = {pos.data(), 3};
    job.out_positions_stride = sizeof(float) * 3;
    CHECK(job.Run());
    ref::Result expected = ref::Skin(mixed.data(), nullptr, indices, 3, weights,
                                     position, nullptr, nullptr);
    CHECK(expected.dual);
    CheckVec3(pos.data(), expected.position, 1e-4f, "zero restored weight");
  }
}

// A positively weighted non-finite matrix isn't rigid: the vertex must take the
// linear fallback path, which produces the same result as the legacy linear
// mode (including its non-finite output, if any).
void TestNonFiniteWeightedMatrix() {
  ozz::math::Float4x4 finite = RigidZ(0.8f, 1.f, 2.f, 3.f);
  ozz::math::Float4x4 non_finite = finite;
  non_finite.cols[0] =
      ozz::math::simd_float4::Load(std::numeric_limits<float>::infinity(), 0.f,
                                   0.f, 0.f);
  std::vector<ozz::math::Float4x4> matrices{finite, non_finite};

  const int vertex_count = 3;
  SimpleMesh mesh;
  mesh.Resize(vertex_count, 2, false, false);
  for (int v = 0; v < vertex_count; ++v) {
    mesh.indices[v * 2 + 0] = 0;
    mesh.indices[v * 2 + 1] = 1;
    mesh.weights[v] = .5f;
    mesh.positions[v * 3 + 0] = 0.1f * v;
    mesh.positions[v * 3 + 1] = 0.6f;
    mesh.positions[v * 3 + 2] = -0.3f;
  }
  std::vector<float> dq_pos(mesh.positions.size());
  std::vector<float> lin_pos(mesh.positions.size());
  SkinningJob dq = MakeJob(mesh, matrices, nullptr, dq_pos.data(), nullptr,
                           nullptr, SkinningJob::kDualQuaternion);
  CHECK(dq.Run());
  SkinningJob linear = MakeJob(mesh, matrices, nullptr, lin_pos.data(), nullptr,
                               nullptr, SkinningJob::kLinear);
  CHECK(linear.Run());
  for (int v = 0; v < vertex_count; ++v) {
    // Both paths must agree bit for bit on this fallback, non-finite inputs
    // included.
    CHECK(std::memcmp(&dq_pos[v * 3], &lin_pos[v * 3], sizeof(float) * 3) == 0);
  }
}

// Validation must remain unchanged for existing callers: the default mode is
// kLinear and the span capacity checks are the historical ones.
void TestLinearModeUnchanged() {  ozz::math::Float4x4 matrices[2];
  matrices[0] = Identity();
  matrices[1] = Identity();
  uint16_t joint_indices[8] = {0};
  float joint_weights[6] = {0.f};
  float in_positions[6] = {0.f};
  float out_positions[6] = {0.f};

  {
    SkinningJob job;
    CHECK(job.mode == SkinningJob::kLinear);
    CHECK(!job.Validate());
    CHECK(!job.Run());
  }
  {
    SkinningJob job;
    job.vertex_count = 2;
    job.influences_count = 2;
    job.joint_matrices = matrices;
    job.joint_indices = {joint_indices, 3};  // One element short.
    job.joint_indices_stride = sizeof(uint16_t) * 2;
    job.joint_weights = {joint_weights, 1};
    job.joint_weights_stride = sizeof(float);
    job.in_positions = in_positions;
    job.in_positions_stride = sizeof(float) * 3;
    job.out_positions = out_positions;
    job.out_positions_stride = sizeof(float) * 3;
    CHECK(!job.Validate());
  }
  {
    // Same capacity, but complete: valid in both modes.
    SkinningJob job;
    job.vertex_count = 1;
    job.influences_count = 2;
    job.joint_matrices = matrices;
    job.joint_indices = {joint_indices, 2};
    job.joint_indices_stride = sizeof(uint16_t) * 2;
    job.joint_weights = {joint_weights, 1};
    job.joint_weights_stride = sizeof(float);
    job.in_positions = in_positions;
    job.in_positions_stride = sizeof(float) * 3;
    job.out_positions = out_positions;
    job.out_positions_stride = sizeof(float) * 3;
    CHECK(job.Validate());
    job.mode = SkinningJob::kDualQuaternion;
    CHECK(job.Validate());
  }
}

}  // namespace

int main() {
  TestLinearModeUnchanged();
  TestLargeTwistRadius();
  TestTranslationAndAxes();
  TestMixedRigidAndNonRigid();
  TestZeroWeightNonRigid();
  TestManyInfluences();
  TestInterleavedInPlace();
  TestInvalidLastVertex();
  TestSingleRigidInfluence();
  TestRigidityAgainstMatrices();
  TestReferenceSelectionAndHemisphere();
  TestRestoredWeightEdges();
  TestNonFiniteWeightedMatrix();

  if (g_failures != 0) {
    std::fprintf(stderr, "OZZ_DQ_TARGET_FAILED failures=%d\n", g_failures);
    return EXIT_FAILURE;
  }
  std::printf("OZZ_DQ_TARGET_OK\n");
  return EXIT_SUCCESS;
}
