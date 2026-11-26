//----------------------------------------------------------------------------//
//                                                                            //
// ozz-animation is hosted at http://github.com/guillaumeblanc/ozz-animation  //
// and distributed under the MIT License (MIT).                               //
//                                                                            //
// Copyright (c) Guillaume Blanc                                              //
//                                                                            //
// Permission is hereby granted, free of charge, to any person obtaining a    //
// copy of this software and associated documentation files (the "Software"), //
// to deal in the Software without restriction, including without limitation  //
// the rights to use, copy, modify, merge, publish, distribute, sublicense,   //
// and/or sell copies of the Software, and to permit persons to whom the      //
// Software is furnished to do so, subject to the following conditions:       //
//                                                                            //
// The above copyright notice and this permission notice shall be included in //
// all copies or substantial portions of the Software.                        //
//                                                                            //
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR //
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,   //
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL    //
// THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER //
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING    //
// FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER        //
// DEALINGS IN THE SOFTWARE.                                                  //
//                                                                            //
//----------------------------------------------------------------------------//

#include "ozz/geometry/runtime/skinning_job.h"

#include <cassert>
#include <cmath>
#include <cstdint>

#include "ozz/base/maths/simd_math.h"

namespace ozz {
namespace geometry {

namespace {

constexpr float kRigidEpsilon = 1e-4f;
constexpr float kMinimumRealLength = 1e-6f;

struct Vec3 {
  float x, y, z;
};

Vec3 Add(const Vec3& _a, const Vec3& _b) {
  return {_a.x + _b.x, _a.y + _b.y, _a.z + _b.z};
}

Vec3 Scale(const Vec3& _v, float _s) {
  return {_v.x * _s, _v.y * _s, _v.z * _s};
}

float Dot(const Vec3& _a, const Vec3& _b) {
  return _a.x * _b.x + _a.y * _b.y + _a.z * _b.z;
}

Vec3 Cross(const Vec3& _a, const Vec3& _b) {
  return {_a.y * _b.z - _a.z * _b.y, _a.z * _b.x - _a.x * _b.z,
          _a.x * _b.y - _a.y * _b.x};
}

struct Quaternion {
  float x, y, z, w;
};

float Dot(const Quaternion& _a, const Quaternion& _b) {
  return _a.x * _b.x + _a.y * _b.y + _a.z * _b.z + _a.w * _b.w;
}

struct DualQuaternion {
  Quaternion real;
  Quaternion dual;
};

Quaternion Scale(const Quaternion& _q, float _s) {
  return {_q.x * _s, _q.y * _s, _q.z * _s, _q.w * _s};
}

Quaternion Add(const Quaternion& _a, const Quaternion& _b) {
  return {_a.x + _b.x, _a.y + _b.y, _a.z + _b.z, _a.w + _b.w};
}

// Extracts a row-major 4x4 matrix from ozz's column-major Float4x4.
void ExtractMatrix(const math::Float4x4& _m, float _r[4][4]) {
  for (int c = 0; c < 4; ++c) {
    float column[4];
    math::StorePtrU(_m.cols[c], column);
    for (int row = 0; row < 4; ++row) {
      _r[row][c] = column[row];
    }
  }
}

bool IsFiniteMatrix(const float _m[4][4]) {
  for (int row = 0; row < 4; ++row) {
    for (int col = 0; col < 4; ++col) {
      if (!std::isfinite(_m[row][col])) {
        return false;
      }
    }
  }
  return true;
}

// Rigid means an affine orthogonal basis with positive orientation.
bool IsRigidMatrix(const math::Float4x4& _matrix) {
  float m[4][4];
  ExtractMatrix(_matrix, m);
  if (!IsFiniteMatrix(m)) {
    return false;
  }

  if (std::fabs(m[3][0]) > kRigidEpsilon ||
      std::fabs(m[3][1]) > kRigidEpsilon ||
      std::fabs(m[3][2]) > kRigidEpsilon ||
      std::fabs(m[3][3] - 1.f) > kRigidEpsilon) {
    return false;
  }

  const Vec3 c0 = {m[0][0], m[1][0], m[2][0]};
  const Vec3 c1 = {m[0][1], m[1][1], m[2][1]};
  const Vec3 c2 = {m[0][2], m[1][2], m[2][2]};

  if (std::fabs(std::sqrt(Dot(c0, c0)) - 1.f) > kRigidEpsilon ||
      std::fabs(std::sqrt(Dot(c1, c1)) - 1.f) > kRigidEpsilon ||
      std::fabs(std::sqrt(Dot(c2, c2)) - 1.f) > kRigidEpsilon) {
    return false;
  }

  if (std::fabs(Dot(c0, c1)) > kRigidEpsilon ||
      std::fabs(Dot(c0, c2)) > kRigidEpsilon ||
      std::fabs(Dot(c1, c2)) > kRigidEpsilon) {
    return false;
  }

  const float determinant = Dot(c0, Cross(c1, c2));
  return std::fabs(determinant - 1.f) <= kRigidEpsilon;
}

// Converts a rigid matrix to a quaternion. The sign is canonicalized by
// inspecting components in w, z, y, x order and making the first non-zero one
// positive.
Quaternion MatrixToQuaternion(const math::Float4x4& _matrix) {
  float m[4][4];
  ExtractMatrix(_matrix, m);

  Quaternion q;
  const float trace = m[0][0] + m[1][1] + m[2][2];
  if (trace > 0.f) {
    const float s = .5f / std::sqrt(trace + 1.f);
    q.w = .25f / s;
    q.x = (m[2][1] - m[1][2]) * s;
    q.y = (m[0][2] - m[2][0]) * s;
    q.z = (m[1][0] - m[0][1]) * s;
  } else if (m[0][0] > m[1][1] && m[0][0] > m[2][2]) {
    const float s = 2.f * std::sqrt(1.f + m[0][0] - m[1][1] - m[2][2]);
    q.w = (m[2][1] - m[1][2]) / s;
    q.x = .25f * s;
    q.y = (m[0][1] + m[1][0]) / s;
    q.z = (m[0][2] + m[2][0]) / s;
  } else if (m[1][1] > m[2][2]) {
    const float s = 2.f * std::sqrt(1.f + m[1][1] - m[0][0] - m[2][2]);
    q.w = (m[0][2] - m[2][0]) / s;
    q.x = (m[0][1] + m[1][0]) / s;
    q.y = .25f * s;
    q.z = (m[1][2] + m[2][1]) / s;
  } else {
    const float s = 2.f * std::sqrt(1.f + m[2][2] - m[0][0] - m[1][1]);
    q.w = (m[1][0] - m[0][1]) / s;
    q.x = (m[0][2] + m[2][0]) / s;
    q.y = (m[1][2] + m[2][1]) / s;
    q.z = .25f * s;
  }

  if (q.w != 0.f) {
    if (q.w < 0.f) q = Scale(q, -1.f);
  } else if (q.z != 0.f) {
    if (q.z < 0.f) q = Scale(q, -1.f);
  } else if (q.y != 0.f) {
    if (q.y < 0.f) q = Scale(q, -1.f);
  } else if (q.x < 0.f) {
    q = Scale(q, -1.f);
  }
  return q;
}

// Builds q + 0.5 * t_quaternion * q, where t is the matrix translation.
DualQuaternion MakeDualQuaternion(const math::Float4x4& _matrix,
                                 const Quaternion& _q) {
  float m[4][4];
  ExtractMatrix(_matrix, m);
  const float tx = m[0][3];
  const float ty = m[1][3];
  const float tz = m[2][3];

  Quaternion d;
  d.w = -.5f * (tx * _q.x + ty * _q.y + tz * _q.z);
  d.x = .5f * (tx * _q.w + ty * _q.z - tz * _q.y);
  d.y = .5f * (-tx * _q.z + ty * _q.w + tz * _q.x);
  d.z = .5f * (tx * _q.y - ty * _q.x + tz * _q.w);
  return {_q, d};
}

Vec3 RotateVector(const Quaternion& _q, const Vec3& _v) {
  const Vec3 axis = {_q.x, _q.y, _q.z};
  const Vec3 t = Scale(Cross(axis, _v), 2.f);
  return Add(_v, Add(Scale(t, _q.w), Cross(axis, t)));
}

// Translation is 2 * dual * conjugate(real), keeping only its vector part.
Vec3 DualTranslation(const Quaternion& _r, const Quaternion& _d) {
  return {2.f * (_r.w * _d.x - _r.x * _d.w + _r.y * _d.z - _r.z * _d.y),
          2.f * (_r.w * _d.y - _r.y * _d.w + _r.z * _d.x - _r.x * _d.z),
          2.f * (_r.w * _d.z - _r.z * _d.w + _r.x * _d.y - _r.y * _d.x)};
}

const float* AdvanceBytes(const float* _p, size_t _stride) {
  if (_p == nullptr) return nullptr;
  return reinterpret_cast<const float*>(reinterpret_cast<uintptr_t>(_p) +
                                        _stride);
}

float* AdvanceBytes(float* _p, size_t _stride) {
  if (_p == nullptr) return nullptr;
  return reinterpret_cast<float*>(reinterpret_cast<uintptr_t>(_p) + _stride);
}

const uint16_t* AdvanceBytes(const uint16_t* _p, size_t _stride) {
  if (_p == nullptr) return nullptr;
  return reinterpret_cast<const uint16_t*>(
      reinterpret_cast<uintptr_t>(_p) + _stride);
}

// Reads and validates every index and weight that the dual quaternion path may
// consume. Matrix bounds are checked through indices here; rigidity is checked
// per positive influence during vertex processing.
bool ValidateDualQuaternionData(const SkinningJob& _job) {
  const size_t palette_size = _job.joint_matrices.size();
  const size_t inverse_transpose_size =
      _job.joint_inverse_transpose_matrices.size();
  const int influence_count = _job.influences_count;
  const int explicit_weight_count = influence_count - 1;

  const uint16_t* indices = _job.joint_indices.begin();
  const float* weights =
      explicit_weight_count > 0 ? _job.joint_weights.begin() : nullptr;

  for (int vertex = 0; vertex < _job.vertex_count; ++vertex) {
    for (int influence = 0; influence < influence_count; ++influence) {
      if (indices[influence] >= palette_size) {
        return false;
      }
      if (inverse_transpose_size != 0 &&
          indices[influence] >= inverse_transpose_size) {
        return false;
      }
    }

    float weight_sum = 0.f;
    for (int influence = 0; influence < explicit_weight_count; ++influence) {
      const float weight = weights[influence];
      if (!std::isfinite(weight) || weight < 0.f || weight > 1.f) {
        return false;
      }
      weight_sum += weight;
    }

    if (!std::isfinite(weight_sum) || weight_sum > 1.f) {
      return false;
    }

    const float last_weight = 1.f - weight_sum;
    if (!std::isfinite(last_weight) || last_weight < 0.f ||
        last_weight > 1.f) {
      return false;
    }

    indices = AdvanceBytes(indices, _job.joint_indices_stride);
    weights = AdvanceBytes(weights, _job.joint_weights_stride);
  }
  return true;
}

float InfluenceWeight(const float* _weights, int _influence,
                      int _influence_count, float _last_weight) {
  if (_influence == _influence_count - 1) {
    return _last_weight;
  }
  return _weights[_influence];
}

float RecoveredLastWeight(const float* _weights, int _influence_count) {
  float sum = 0.f;
  for (int i = 0; i < _influence_count - 1; ++i) {
    sum += _weights[i];
  }
  return 1.f - sum;
}

Vec3 ReadVec3(const float* _p) { return {_p[0], _p[1], _p[2]}; }

void WriteVec3(float* _p, const Vec3& _v) {
  _p[0] = _v.x;
  _p[1] = _v.y;
  _p[2] = _v.z;
}

using LinearSkinningFunction = void (*)(const SkinningJob&);

// Invokes the exact legacy linear skinning variant for one vertex. The job is
// a stack-local view onto the vertex currently being processed.
void InvokeLinearVertex(
    const SkinningJob& _job, LinearSkinningFunction _function,
    const uint16_t* _indices, const float* _weights,
    const float* _in_positions, float* _out_positions,
    const float* _in_normals, float* _out_normals,
    const float* _in_tangents, float* _out_tangents) {
  SkinningJob one = _job;
  one.vertex_count = 1;
  one.skinning_mode = SkinningJob::kLinear;
  one.joint_indices =
      span<const uint16_t>(_indices, _job.influences_count);
  if (_job.influences_count > 1) {
    one.joint_weights =
        span<const float>(_weights, _job.influences_count - 1);
  } else {
    one.joint_weights = {};
  }

  one.in_positions = span<const float>(_in_positions, 3);
  one.out_positions = span<float>(_out_positions, 3);
  if (_in_normals != nullptr) {
    one.in_normals = span<const float>(_in_normals, 3);
    one.out_normals = span<float>(_out_normals, 3);
  } else {
    one.in_normals = {};
    one.out_normals = {};
  }
  if (_in_tangents != nullptr) {
    one.in_tangents = span<const float>(_in_tangents, 3);
    one.out_tangents = span<float>(_out_tangents, 3);
  } else {
    one.in_tangents = {};
    one.out_tangents = {};
  }

  _function(one);
}

bool RunDualQuaternion(const SkinningJob& _job,
                       LinearSkinningFunction _linear_function) {
  if (!ValidateDualQuaternionData(_job)) {
    return false;
  }

  const int count = _job.influences_count;
  const uint16_t* indices = _job.joint_indices.begin();
  const float* weights = count > 1 ? _job.joint_weights.begin() : nullptr;

  const float* in_positions = _job.in_positions.begin();
  float* out_positions = _job.out_positions.begin();
  const float* in_normals =
      _job.in_normals.empty() ? nullptr : _job.in_normals.begin();
  float* out_normals =
      _job.out_normals.empty() ? nullptr : _job.out_normals.begin();
  const float* in_tangents =
      _job.in_tangents.empty() ? nullptr : _job.in_tangents.begin();
  float* out_tangents =
      _job.out_tangents.empty() ? nullptr : _job.out_tangents.begin();

  for (int vertex = 0; vertex < _job.vertex_count; ++vertex) {
    const float last_weight = RecoveredLastWeight(weights, count);

    int reference = -1;
    uint16_t reference_index = 0;
    float reference_weight = -1.f;
    bool rigid = true;

    for (int i = 0; i < count; ++i) {
      const float w = InfluenceWeight(weights, i, count, last_weight);
      if (!(w > 0.f)) {
        continue;
      }

      const uint16_t index = indices[i];
      if (w > reference_weight ||
          (w == reference_weight && index < reference_index)) {
        reference_weight = w;
        reference_index = index;
        reference = i;
      }

      if (!IsRigidMatrix(_job.joint_matrices[index])) {
        rigid = false;
      }
    }

    if (!rigid || reference < 0) {
      InvokeLinearVertex(_job, _linear_function, indices, weights,
                         in_positions, out_positions, in_normals, out_normals,
                         in_tangents, out_tangents);
    } else {
      const Quaternion reference_q =
          MatrixToQuaternion(_job.joint_matrices[reference_index]);

      Quaternion real = {0.f, 0.f, 0.f, 0.f};
      Quaternion dual = {0.f, 0.f, 0.f, 0.f};

      for (int i = 0; i < count; ++i) {
        const float w = InfluenceWeight(weights, i, count, last_weight);
        if (!(w > 0.f)) {
          continue;
        }

        const uint16_t index = indices[i];
        Quaternion q = MatrixToQuaternion(_job.joint_matrices[index]);

        // Put every rotation in the reference rotation's hemisphere. Dot
        // product exactly equal to zero intentionally causes no flip.
        if (Dot(q, reference_q) < 0.f) {
          q = Scale(q, -1.f);
        }

        const DualQuaternion dq =
            MakeDualQuaternion(_job.joint_matrices[index], q);
        real.x += w * dq.real.x;
        real.y += w * dq.real.y;
        real.z += w * dq.real.z;
        real.w += w * dq.real.w;
        dual.x += w * dq.dual.x;
        dual.y += w * dq.dual.y;
        dual.z += w * dq.dual.z;
        dual.w += w * dq.dual.w;
      }

      const float real_length = std::sqrt(Dot(real, real));
      if (real_length < kMinimumRealLength) {
        InvokeLinearVertex(_job, _linear_function, indices, weights,
                           in_positions, out_positions, in_normals,
                           out_normals, in_tangents, out_tangents);
      } else {
        const float inverse_length = 1.f / real_length;
        real = Scale(real, inverse_length);
        dual = Scale(dual, inverse_length);

        // Remove the remaining real/dual dot product to obtain a unit dual
        // quaternion.
        dual = Add(dual, Scale(real, -Dot(real, dual)));

        const Vec3 translation = DualTranslation(real, dual);

        const Vec3 p = ReadVec3(in_positions);
        const Vec3 rotated_p = RotateVector(real, p);
        WriteVec3(out_positions, Add(rotated_p, translation));

        if (in_normals != nullptr) {
          WriteVec3(out_normals, RotateVector(real, ReadVec3(in_normals)));
        }
        if (in_tangents != nullptr) {
          WriteVec3(out_tangents, RotateVector(real, ReadVec3(in_tangents)));
        }
      }
    }

    indices = AdvanceBytes(indices, _job.joint_indices_stride);
    weights = AdvanceBytes(weights, _job.joint_weights_stride);
    in_positions = AdvanceBytes(in_positions, _job.in_positions_stride);
    out_positions = AdvanceBytes(out_positions, _job.out_positions_stride);
    in_normals = AdvanceBytes(in_normals, _job.in_normals_stride);
    out_normals = AdvanceBytes(out_normals, _job.out_normals_stride);
    in_tangents = AdvanceBytes(in_tangents, _job.in_tangents_stride);
    out_tangents = AdvanceBytes(out_tangents, _job.out_tangents_stride);
  }

  return true;
}

}  // namespace

SkinningJob::SkinningJob()
    : vertex_count(0),
      skinning_mode(kLinear),
      influences_count(0),
      joint_indices_stride(0),
      joint_weights_stride(0),
      in_positions_stride(0),
      in_normals_stride(0),
      in_tangents_stride(0),
      out_positions_stride(0),
      out_normals_stride(0),
      out_tangents_stride(0) {}

bool SkinningJob::Validate() const {
  // Start validation of all parameters.
  bool valid = true;

  // Checks influences bounds.
  valid &= influences_count > 0;

  // Checks joints matrices, required.
  valid &= !joint_matrices.empty();

  // Prepares local variables used to compute buffer size.
  const int vertex_count_minus_1 = vertex_count > 0 ? vertex_count - 1 : 0;
  const int vertex_count_at_least_1 = vertex_count > 0;

  // Checks indices, required.
  valid &= joint_indices.size_bytes() >=
           joint_indices_stride * vertex_count_minus_1 +
               sizeof(uint16_t) * influences_count * vertex_count_at_least_1;

  // Checks weights, required if influences_count > 1.
  if (influences_count != 1) {
    valid &=
        joint_weights.size_bytes() >=
        joint_weights_stride * vertex_count_minus_1 +
            sizeof(float) * (influences_count - 1) * vertex_count_at_least_1;
  }

  // Checks positions, mandatory.
  valid &= in_positions.size_bytes() >=
           in_positions_stride * vertex_count_minus_1 +
               sizeof(float) * 3 * vertex_count_at_least_1;
  valid &= !out_positions.empty();
  valid &= out_positions.size_bytes() >=
           out_positions_stride * vertex_count_minus_1 +
               sizeof(float) * 3 * vertex_count_at_least_1;

  // Checks normals, optional.
  if (!in_normals.empty()) {
    valid &= in_normals.size_bytes() >=
             in_normals_stride * vertex_count_minus_1 +
                 sizeof(float) * 3 * vertex_count_at_least_1;
    valid &= !out_normals.empty();
    valid &= out_normals.size_bytes() >=
             out_normals_stride * vertex_count_minus_1 +
                 sizeof(float) * 3 * vertex_count_at_least_1;

    // Checks tangents, optional but requires normals.
    if (!in_tangents.empty()) {
      valid &= in_tangents.size_bytes() >=
               in_tangents_stride * vertex_count_minus_1 +
                   sizeof(float) * 3 * vertex_count_at_least_1;
      valid &= !out_tangents.empty();
      valid &= out_tangents.size_bytes() >=
               out_tangents_stride * vertex_count_minus_1 +
                   sizeof(float) * 3 * vertex_count_at_least_1;
    }
  } else {
    // Tangents are not supported if normals are not there.
    valid &= in_tangents.empty();
  }

  return valid;
}

// For performance optimization reasons, every skinning variants (positions,
// positions + normals, 1 to n influences...) are implemented as separate
// specialized functions.
// To cope with the error prone aspect of implementing every function, we
// define a skeleton code (SKINNING_FN) for the skinning loop, which internally
// calls MACRO that are shared or specialized according to skinning variants.

// Defines the skeleton code for the per vertex skinning loop.
#define SKINNING_FN(_type, _it, _inf)                                        \
  void SKINNING_FN_NAME(_type, _it, _inf)(const SkinningJob& _job) {         \
    ASSERT_##_type() ASSERT_##_it() INIT_##_type() INIT_W##_inf()            \
        const int loops = _job.vertex_count - 1;                             \
    for (int i = 0; i < loops; ++i) {                                        \
      PREPARE_##_inf##_INNER(_it) TRANSFORM_##_type##_INNER() NEXT_##_type() \
          NEXT_W##_inf()                                                     \
    }                                                                        \
    PREPARE_##_inf##_OUTER(_it) TRANSFORM_##_type##_OUTER()                  \
  }

// Defines skinning function name.
#define SKINNING_FN_NAME(_type, _it, _inf) Skinning##_type##_it##_inf

// Implements pre-conditions assertions.
#define ASSERT_P()                                          \
  assert(_job.vertex_count && !_job.in_positions.empty() && \
         _job.in_normals.empty());

#define ASSERT_PN()                                         \
  assert(_job.vertex_count && !_job.in_positions.empty() && \
         !_job.in_normals.empty() && _job.in_tangents.empty());

#define ASSERT_PNT()                                        \
  assert(_job.vertex_count && !_job.in_positions.empty() && \
         !_job.in_normals.empty() && !_job.in_tangents.empty());

#define ASSERT_NOIT()

#define ASSERT_IT() assert(!_job.joint_inverse_transpose_matrices.empty());

// Implements loop initializations for positions, ...
#define INIT_P()                                              \
  const uint16_t* joint_indices = _job.joint_indices.begin(); \
  const float* in_positions = _job.in_positions.begin();      \
  float* out_positions = _job.out_positions.begin();

#define INIT_PN()                                    \
  INIT_P();                                          \
  const float* in_normals = _job.in_normals.begin(); \
  float* out_normals = _job.out_normals.begin();

#define INIT_PNT()                                     \
  INIT_PN();                                           \
  const float* in_tangents = _job.in_tangents.begin(); \
  float* out_tangents = _job.out_tangents.begin();

// Implements loop initializations for weights.
// Note that if the number of influences per vertex is 1, then there's no weight
// as it's implicitly 1.
#define INIT_W1()

#define INIT_W2()                                        \
  const math::SimdFloat4 one = math::simd_float4::one(); \
  const float* joint_weights = _job.joint_weights.begin();

#define INIT_W3() INIT_W2()

#define INIT_W4() INIT_W2()

#define INIT_WN() INIT_W2()

// Implements pointer striding.
#define NEXT(_type, _current, _stride) \
  reinterpret_cast<_type>(reinterpret_cast<uintptr_t>(_current) + _stride)

#define NEXT_W1()

#define NEXT_W2() \
  joint_weights = NEXT(const float*, joint_weights, _job.joint_weights_stride);

#define NEXT_W3() NEXT_W2()

#define NEXT_W4() NEXT_W2()

#define NEXT_WN() NEXT_W2()

#define NEXT_P()                                                             \
  joint_indices =                                                            \
      NEXT(const uint16_t*, joint_indices, _job.joint_indices_stride);       \
  in_positions = NEXT(const float*, in_positions, _job.in_positions_stride); \
  out_positions = NEXT(float*, out_positions, _job.out_positions_stride);

#define NEXT_PN()                                                      \
  NEXT_P();                                                            \
  in_normals = NEXT(const float*, in_normals, _job.in_normals_stride); \
  out_normals = NEXT(float*, out_normals, _job.out_normals_stride);

#define NEXT_PNT()                                                        \
  NEXT_PN();                                                              \
  in_tangents = NEXT(const float*, in_tangents, _job.in_tangents_stride); \
  out_tangents = NEXT(float*, out_tangents, _job.out_tangents_stride);

// Implements weighted matrix preparation.
// _INNER functions are intended to be used inside the vertex loop. They take
// advantage of the fact that the buffers they are reading from contain enough
// remaining data to use more optimized SIMD load functions. At the opposite,
// _OUTER functions restrict access to data that are sure to be readable from
// the buffer.
#define PREPARE_1_INNER(_it)                                 \
  const uint16_t i0 = joint_indices[0];                      \
  const math::Float4x4& transform = _job.joint_matrices[i0]; \
  PREPARE_##_it##_1()

#define PREPARE_1_OUTER(_it) PREPARE_1_INNER(_it)

#define PREPARE_NOIT()                            \
  const math::Float4x4& it_transform = transform; \
  (void)it_transform;

#define PREPARE_NOIT_1() PREPARE_NOIT()

#define PREPARE_IT_1()                 \
  const math::Float4x4& it_transform = \
      _job.joint_inverse_transpose_matrices[i0];

#define PREPARE_2_INNER(_it)                                                   \
  const math::SimdFloat4 w0 = math::simd_float4::Load1PtrU(joint_weights + 0); \
  const uint16_t i0 = joint_indices[0];                                        \
  const uint16_t i1 = joint_indices[1];                                        \
  const math::Float4x4& m0 = _job.joint_matrices[i0];                          \
  const math::Float4x4& m1 = _job.joint_matrices[i1];                          \
  const math::SimdFloat4 w1 = one - w0;                                        \
  const math::Float4x4 transform =                                             \
      math::ColumnMultiply(m0, w0) + math::ColumnMultiply(m1, w1);             \
  PREPARE_##_it##_2()

#define PREPARE_NOIT_2() PREPARE_NOIT()

#define PREPARE_IT_2()                                                    \
  const math::Float4x4& mit0 = _job.joint_inverse_transpose_matrices[i0]; \
  const math::Float4x4& mit1 = _job.joint_inverse_transpose_matrices[i1]; \
  const math::Float4x4 it_transform =                                     \
      math::ColumnMultiply(mit0, w0) + math::ColumnMultiply(mit1, w1);

#define PREPARE_2_OUTER(_it) PREPARE_2_INNER(_it)

#define PREPARE_3_CONCAT(_it)                                     \
  const uint16_t i0 = joint_indices[0];                           \
  const uint16_t i1 = joint_indices[1];                           \
  const uint16_t i2 = joint_indices[2];                           \
  const math::Float4x4& m0 = _job.joint_matrices[i0];             \
  const math::Float4x4& m1 = _job.joint_matrices[i1];             \
  const math::Float4x4& m2 = _job.joint_matrices[i2];             \
  const math::SimdFloat4 w2 = one - (w0 + w1);                    \
  const math::Float4x4 transform = math::ColumnMultiply(m0, w0) + \
                                   math::ColumnMultiply(m1, w1) + \
                                   math::ColumnMultiply(m2, w2);  \
  PREPARE_##_it##_3()

#define PREPARE_NOIT_3() PREPARE_NOIT()

#define PREPARE_IT_3()                                                    \
  const math::Float4x4& mit0 = _job.joint_inverse_transpose_matrices[i0]; \
  const math::Float4x4& mit1 = _job.joint_inverse_transpose_matrices[i1]; \
  const math::Float4x4& mit2 = _job.joint_inverse_transpose_matrices[i2]; \
  const math::Float4x4 it_transform = math::ColumnMultiply(mit0, w0) +    \
                                      math::ColumnMultiply(mit1, w1) +    \
                                      math::ColumnMultiply(mit2, w2);

#define PREPARE_3_INNER(_it)                                             \
  const math::SimdFloat4 w = math::simd_float4::LoadPtrU(joint_weights); \
  const math::SimdFloat4 w0 = math::SplatX(w);                           \
  const math::SimdFloat4 w1 = math::SplatY(w);                           \
  PREPARE_3_CONCAT(_it)

#define PREPARE_3_OUTER(_it)                                                   \
  const math::SimdFloat4 w0 = math::simd_float4::Load1PtrU(joint_weights + 0); \
  const math::SimdFloat4 w1 = math::simd_float4::Load1PtrU(joint_weights + 1); \
  PREPARE_3_CONCAT(_it)

#define PREPARE_4_CONCAT(_it)                                       \
  const uint16_t i0 = joint_indices[0];                             \
  const uint16_t i1 = joint_indices[1];                             \
  const uint16_t i2 = joint_indices[2];                             \
  const uint16_t i3 = joint_indices[3];                             \
  const math::Float4x4& m0 = _job.joint_matrices[i0];               \
  const math::Float4x4& m1 = _job.joint_matrices[i1];               \
  const math::Float4x4& m2 = _job.joint_matrices[i2];               \
  const math::Float4x4& m3 = _job.joint_matrices[i3];               \
  const math::SimdFloat4 w3 = one - (w0 + w1 + w2);                 \
  const math::Float4x4 transform =                                  \
      math::ColumnMultiply(m0, w0) + math::ColumnMultiply(m1, w1) + \
      math::ColumnMultiply(m2, w2) + math::ColumnMultiply(m3, w3);  \
  PREPARE_##_it##_4()

#define PREPARE_NOIT_4() PREPARE_NOIT()

#define PREPARE_IT_4()                                                    \
  const math::Float4x4& mit0 = _job.joint_inverse_transpose_matrices[i0]; \
  const math::Float4x4& mit1 = _job.joint_inverse_transpose_matrices[i1]; \
  const math::Float4x4& mit2 = _job.joint_inverse_transpose_matrices[i2]; \
  const math::Float4x4& mit3 = _job.joint_inverse_transpose_matrices[i3]; \
  const math::Float4x4 it_transform =                                     \
      math::ColumnMultiply(mit0, w0) + math::ColumnMultiply(mit1, w1) +   \
      math::ColumnMultiply(mit2, w2) + math::ColumnMultiply(mit3, w3);

#define PREPARE_4_INNER(_it)                                             \
  const math::SimdFloat4 w = math::simd_float4::LoadPtrU(joint_weights); \
  const math::SimdFloat4 w0 = math::SplatX(w);                           \
  const math::SimdFloat4 w1 = math::SplatY(w);                           \
  const math::SimdFloat4 w2 = math::SplatZ(w);                           \
  PREPARE_4_CONCAT(_it)

#define PREPARE_4_OUTER(_it)                                                   \
  const math::SimdFloat4 w0 = math::simd_float4::Load1PtrU(joint_weights + 0); \
  const math::SimdFloat4 w1 = math::simd_float4::Load1PtrU(joint_weights + 1); \
  const math::SimdFloat4 w2 = math::simd_float4::Load1PtrU(joint_weights + 2); \
  PREPARE_4_CONCAT(_it)

#define PREPARE_NOIT_N()                                                     \
  math::SimdFloat4 wsum = math::simd_float4::Load1PtrU(joint_weights + 0);   \
  math::Float4x4 transform =                                                 \
      math::ColumnMultiply(_job.joint_matrices[joint_indices[0]], wsum);     \
  const int last = _job.influences_count - 1;                                \
  for (int j = 1; j < last; ++j) {                                           \
    const math::SimdFloat4 w =                                               \
        math::simd_float4::Load1PtrU(joint_weights + j);                     \
    wsum = wsum + w;                                                         \
    transform = transform + math::ColumnMultiply(                            \
                                _job.joint_matrices[joint_indices[j]], w);   \
  }                                                                          \
  transform =                                                                \
      transform + math::ColumnMultiply(                                      \
                      _job.joint_matrices[joint_indices[last]], one - wsum); \
  PREPARE_NOIT()

#define PREPARE_IT_N()                                                        \
  math::SimdFloat4 wsum = math::simd_float4::Load1PtrU(joint_weights + 0);    \
  const uint16_t i0 = joint_indices[0];                                       \
  math::Float4x4 transform =                                                  \
      math::ColumnMultiply(_job.joint_matrices[i0], wsum);                    \
  math::Float4x4 it_transform =                                               \
      math::ColumnMultiply(_job.joint_inverse_transpose_matrices[i0], wsum);  \
  const int last = _job.influences_count - 1;                                 \
  for (int j = 1; j < last; ++j) {                                            \
    const uint16_t ij = joint_indices[j];                                     \
    const math::SimdFloat4 w =                                                \
        math::simd_float4::Load1PtrU(joint_weights + j);                      \
    wsum = wsum + w;                                                          \
    transform = transform + math::ColumnMultiply(_job.joint_matrices[ij], w); \
    it_transform =                                                            \
        it_transform +                                                        \
        math::ColumnMultiply(_job.joint_inverse_transpose_matrices[ij], w);   \
  }                                                                           \
  const math::SimdFloat4 wlast = one - wsum;                                  \
  const int ilast = joint_indices[last];                                      \
  transform =                                                                 \
      transform + math::ColumnMultiply(_job.joint_matrices[ilast], wlast);    \
  it_transform =                                                              \
      it_transform + math::ColumnMultiply(                                    \
                         _job.joint_inverse_transpose_matrices[ilast], wlast);

#define PREPARE_N_INNER(_it) PREPARE_##_it##_N()

#define PREPARE_N_OUTER(_it) PREPARE_##_it##_N()

// Implement point and vector transformation. _INNER and _OUTER have the same
// meaning as defined for the PREPARE functions.
#define TRANSFORM_P_INNER()                                                \
  const math::SimdFloat4 in_p = math::simd_float4::LoadPtrU(in_positions); \
  const math::SimdFloat4 out_p = TransformPoint(transform, in_p);          \
  math::Store3PtrU(out_p, out_positions);

#define TRANSFORM_PN_INNER()                                             \
  TRANSFORM_P_INNER();                                                   \
  const math::SimdFloat4 in_n = math::simd_float4::LoadPtrU(in_normals); \
  const math::SimdFloat4 out_n = TransformVector(it_transform, in_n);    \
  math::Store3PtrU(out_n, out_normals);

#define TRANSFORM_PNT_INNER()                                             \
  TRANSFORM_PN_INNER();                                                   \
  const math::SimdFloat4 in_t = math::simd_float4::LoadPtrU(in_tangents); \
  const math::SimdFloat4 out_t = TransformVector(it_transform, in_t);     \
  math::Store3PtrU(out_t, out_tangents);

#define TRANSFORM_P_OUTER()                                                 \
  const math::SimdFloat4 in_p = math::simd_float4::Load3PtrU(in_positions); \
  const math::SimdFloat4 out_p = TransformPoint(transform, in_p);           \
  math::Store3PtrU(out_p, out_positions);

#define TRANSFORM_PN_OUTER()                                              \
  TRANSFORM_P_OUTER();                                                    \
  const math::SimdFloat4 in_n = math::simd_float4::Load3PtrU(in_normals); \
  const math::SimdFloat4 out_n = TransformVector(it_transform, in_n);     \
  math::Store3PtrU(out_n, out_normals);

#define TRANSFORM_PNT_OUTER()                                              \
  TRANSFORM_PN_OUTER();                                                    \
  const math::SimdFloat4 in_t = math::simd_float4::Load3PtrU(in_tangents); \
  const math::SimdFloat4 out_t = TransformVector(it_transform, in_t);      \
  math::Store3PtrU(out_t, out_tangents);

// Instantiates all skinning function variants.
SKINNING_FN(P, NOIT, 1)
SKINNING_FN(PN, NOIT, 1)
SKINNING_FN(PNT, NOIT, 1)
SKINNING_FN(PN, IT, 1)
SKINNING_FN(PNT, IT, 1)
SKINNING_FN(P, NOIT, 2)
SKINNING_FN(PN, NOIT, 2)
SKINNING_FN(PNT, NOIT, 2)
SKINNING_FN(PN, IT, 2)
SKINNING_FN(PNT, IT, 2)
SKINNING_FN(P, NOIT, 3)
SKINNING_FN(PN, NOIT, 3)
SKINNING_FN(PNT, NOIT, 3)
SKINNING_FN(PN, IT, 3)
SKINNING_FN(PNT, IT, 3)
SKINNING_FN(P, NOIT, 4)
SKINNING_FN(PN, NOIT, 4)
SKINNING_FN(PNT, NOIT, 4)
SKINNING_FN(PN, IT, 4)
SKINNING_FN(PNT, IT, 4)
SKINNING_FN(P, NOIT, N)
SKINNING_FN(PN, NOIT, N)
SKINNING_FN(PNT, NOIT, N)
SKINNING_FN(PN, IT, N)
SKINNING_FN(PNT, IT, N)

// Defines a matrix of skinning function pointers. This matrix will then be
// indexed according to skinning jobs parameters.
typedef void (*SkiningFct)(const SkinningJob&);
static const SkiningFct kSkinningFct[2][5][3] = {
    {
        {&SKINNING_FN_NAME(P, NOIT, 1), &SKINNING_FN_NAME(PN, NOIT, 1),
         &SKINNING_FN_NAME(PNT, NOIT, 1)},
        {&SKINNING_FN_NAME(P, NOIT, 2), &SKINNING_FN_NAME(PN, NOIT, 2),
         &SKINNING_FN_NAME(PNT, NOIT, 2)},
        {&SKINNING_FN_NAME(P, NOIT, 3), &SKINNING_FN_NAME(PN, NOIT, 3),
         &SKINNING_FN_NAME(PNT, NOIT, 3)},
        {&SKINNING_FN_NAME(P, NOIT, 4), &SKINNING_FN_NAME(PN, NOIT, 4),
         &SKINNING_FN_NAME(PNT, NOIT, 4)},
        {&SKINNING_FN_NAME(P, NOIT, N), &SKINNING_FN_NAME(PN, NOIT, N),
         &SKINNING_FN_NAME(PNT, NOIT, N)},
    },
    {
        {&SKINNING_FN_NAME(P, NOIT, 1), &SKINNING_FN_NAME(PN, IT, 1),
         &SKINNING_FN_NAME(PNT, IT, 1)},
        {&SKINNING_FN_NAME(P, NOIT, 2), &SKINNING_FN_NAME(PN, IT, 2),
         &SKINNING_FN_NAME(PNT, IT, 2)},
        {&SKINNING_FN_NAME(P, NOIT, 3), &SKINNING_FN_NAME(PN, IT, 3),
         &SKINNING_FN_NAME(PNT, IT, 3)},
        {&SKINNING_FN_NAME(P, NOIT, 4), &SKINNING_FN_NAME(PN, IT, 4),
         &SKINNING_FN_NAME(PNT, IT, 4)},
        {&SKINNING_FN_NAME(P, NOIT, N), &SKINNING_FN_NAME(PN, IT, N),
         &SKINNING_FN_NAME(PNT, IT, N)},
    }};

// Implements job Run function.
bool SkinningJob::Run() const {
  // Exit with an error if job is invalid.
  if (!Validate()) {
    return false;
  }

  // Early out if no vertex. This isn't an error.
  // Skinning function algorithm doesn't support the case.
  if (vertex_count == 0) {
    return true;
  }

  // Find skinning function index. The same selected linear variant is reused
  // by dual quaternion vertices that need the historical fallback path.
  const size_t it = !joint_inverse_transpose_matrices.empty();
  assert(it < OZZ_ARRAY_SIZE(kSkinningFct));
  const size_t inf =
      static_cast<size_t>(influences_count) > OZZ_ARRAY_SIZE(kSkinningFct[0])
          ? OZZ_ARRAY_SIZE(kSkinningFct[0]) - 1
          : influences_count - 1;
  assert(inf < OZZ_ARRAY_SIZE(kSkinningFct[0]));
  const size_t fct = !in_normals.empty() + !in_tangents.empty();
  assert(fct < OZZ_ARRAY_SIZE(kSkinningFct[0][0]));
  const SkiningFct skinning_function = kSkinningFct[it][inf][fct];

  if (skinning_mode == kDualQuaternion) {
    return RunDualQuaternion(*this, skinning_function);
  }

  // Calls skinning function. Cannot fail because job is valid.
  skinning_function(*this);

  return true;
}
}  // namespace geometry
}  // namespace ozz
