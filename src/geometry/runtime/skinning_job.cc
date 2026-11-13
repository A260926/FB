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

#include "ozz/base/maths/simd_math.h"

namespace ozz {
namespace geometry {

SkinningJob::SkinningJob()
    : vertex_count(0),
      mode(kLinear),
      influences_count(0),
      joint_indices_stride(0),
      joint_weights_stride(0),
      in_positions_stride(0),
      in_normals_stride(0),
      in_tangents_stride(0),
      out_positions_stride(0),
      out_normals_stride(0),
      out_tangents_stride(0) {}

namespace {

// Absolute tolerance used by the dual quaternion path to qualify a matrix as
// rigid. It applies independently to the bottom row, to every linear column
// length and to every pair of linear columns orthogonality, as well as to the
// determinant.
const float kRigidTolerance = 1e-4f;

// Minimum length of the blended dual quaternion real part. Below this value the
// vertex falls back to linear blend skinning, avoiding a division by (almost)
// zero.
const float kRealPartLengthTolerance = 1e-6f;

// Scalar quaternion used by the dual quaternion skinning path. Using scalar
// arithmetic here keeps the implementation independent from the SIMD
// specialization and doesn't require any heap allocation.
struct Quat {
  float x, y, z, w;
};

// Extracts a Float4x4 column component.
inline float MatrixComponent(const math::Float4x4& _m, int _column, int _row) {
  switch (_row) {
    case 0:
      return math::GetX(_m.cols[_column]);
    case 1:
      return math::GetY(_m.cols[_column]);
    case 2:
      return math::GetZ(_m.cols[_column]);
    default:
      return math::GetW(_m.cols[_column]);
  }
}

// Returns true if all the matrix components are finite.
inline bool IsFiniteMatrix(const math::Float4x4& _m) {
  for (int c = 0; c < 4; ++c) {
    // Be exhaustive, including the w component of the linear columns.
    if (!std::isfinite(math::GetX(_m.cols[c])) ||
        !std::isfinite(math::GetY(_m.cols[c])) ||
        !std::isfinite(math::GetZ(_m.cols[c])) ||
        !std::isfinite(math::GetW(_m.cols[c]))) {
      return false;
    }
  }
  return true;
}

// Returns true if _m is a rigid transformation, within kRigidTolerance:
// - all components are finite,
// - the last row is (0,0,0,1),
// - the three linear columns have unit length,
// - the three linear columns are pairwise orthogonal,
// - the linear part determinant is +1.
inline bool IsRigidMatrix(const math::Float4x4& _m) {
  if (!IsFiniteMatrix(_m)) {
    return false;
  }

  // Last row must be (0,0,0,1).
  if (!(std::abs(MatrixComponent(_m, 0, 3)) <= kRigidTolerance) ||
      !(std::abs(MatrixComponent(_m, 1, 3)) <= kRigidTolerance) ||
      !(std::abs(MatrixComponent(_m, 2, 3)) <= kRigidTolerance) ||
      !(std::abs(MatrixComponent(_m, 3, 3) - 1.f) <= kRigidTolerance)) {
    return false;
  }

  float columns[3][3];
  for (int c = 0; c < 3; ++c) {
    columns[c][0] = MatrixComponent(_m, c, 0);
    columns[c][1] = MatrixComponent(_m, c, 1);
    columns[c][2] = MatrixComponent(_m, c, 2);

    // Unit length.
    const float length_sqr = columns[c][0] * columns[c][0] +
                             columns[c][1] * columns[c][1] +
                             columns[c][2] * columns[c][2];
    const float length = std::sqrt(length_sqr);
    if (!(std::abs(length - 1.f) <= kRigidTolerance)) {
      return false;
    }
  }

  // Pairwise orthogonality.
  for (int i = 0; i < 3; ++i) {
    for (int j = i + 1; j < 3; ++j) {
      const float dot = columns[i][0] * columns[j][0] +
                        columns[i][1] * columns[j][1] +
                        columns[i][2] * columns[j][2];
      if (!(std::abs(dot) <= kRigidTolerance)) {
        return false;
      }
    }
  }

  // Determinant of the linear part.
  const float determinant =
      columns[0][0] * (columns[1][1] * columns[2][2] -
                       columns[1][2] * columns[2][1]) -
      columns[1][0] * (columns[0][1] * columns[2][2] -
                       columns[0][2] * columns[2][1]) +
      columns[2][0] * (columns[0][1] * columns[1][2] -
                       columns[0][2] * columns[1][1]);
  if (!(std::abs(determinant - 1.f) <= kRigidTolerance)) {
    return false;
  }

  return true;
}

// Builds the unit quaternion representing the rotation of a rigid matrix.
// The quaternion is normalized and its sign is fixed: among the w, z, y and x
// components, in this order, the first non-zero one is made positive.
// The extraction is done with scalar arithmetic, independently from the SIMD
// matrix to quaternion helper, so that it is fully deterministic and doesn't
// rely on any normalization pre-condition.
inline bool BuildRotation(const math::Float4x4& _m, float* _x, float* _y,
                          float* _z, float* _w) {
  // Linear part, column major: r[_row][_column].
  const float r00 = MatrixComponent(_m, 0, 0);
  const float r10 = MatrixComponent(_m, 0, 1);
  const float r20 = MatrixComponent(_m, 0, 2);
  const float r01 = MatrixComponent(_m, 1, 0);
  const float r11 = MatrixComponent(_m, 1, 1);
  const float r21 = MatrixComponent(_m, 1, 2);
  const float r02 = MatrixComponent(_m, 2, 0);
  const float r12 = MatrixComponent(_m, 2, 1);
  const float r22 = MatrixComponent(_m, 2, 2);

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

  // Normalizes the quaternion.
  const float length_sqr = x * x + y * y + z * z + w * w;
  if (!(length_sqr >= kRealPartLengthTolerance * kRealPartLengthTolerance)) {
    return false;
  }
  const float inv_length = 1.f / std::sqrt(length_sqr);
  x *= inv_length;
  y *= inv_length;
  z *= inv_length;
  w *= inv_length;

  // Sign canonization, in w, z, y, x order.
  float first_non_zero = w;
  if (first_non_zero == 0.f) {
    first_non_zero = z;
  }
  if (first_non_zero == 0.f) {
    first_non_zero = y;
  }
  if (first_non_zero == 0.f) {
    first_non_zero = x;
  }
  if (first_non_zero < 0.f) {
    x = -x;
    y = -y;
    z = -z;
    w = -w;
  }

  *_x = x;
  *_y = y;
  *_z = z;
  *_w = w;
  return true;
}

// Multiplies two scalar quaternions.
inline Quat Multiply(const float _ax, const float _ay, const float _az,
                     const float _aw, const float _bx, const float _by,
                     const float _bz, const float _bw) {
  Quat ret;
  ret.x = _aw * _bx + _ax * _bw + _ay * _bz - _az * _by;
  ret.y = _aw * _by + _ay * _bw + _az * _bx - _ax * _bz;
  ret.z = _aw * _bz + _az * _bw + _ax * _by - _ay * _bx;
  ret.w = _aw * _bw - _ax * _bx - _ay * _by - _az * _bz;
  return ret;
}

// Rotates vector _v by unit quaternion (_x,_y,_z,_w).
inline void RotateVector(float _x, float _y, float _z, float _w, float _vx,
                         float _vy, float _vz, float* _out_x, float* _out_y,
                         float* _out_z) {
  // t = 2 * cross(q_xyz, v)
  const float tx = 2.f * (_y * _vz - _z * _vy);
  const float ty = 2.f * (_z * _vx - _x * _vz);
  const float tz = 2.f * (_x * _vy - _y * _vx);

  // v' = v + w * t + cross(q_xyz, t)
  *_out_x = _vx + _w * tx + (_y * tz - _z * ty);
  *_out_y = _vy + _w * ty + (_z * tx - _x * tz);
  *_out_z = _vz + _w * tz + (_x * ty - _y * tx);
}

// Validates the per-vertex input data the dual quaternion path reads (indices
// and weights). The job spans have already been validated when this is called,
// so reading these values is safe. Returns false on the first invalid value.
bool ValidateDualQuaternionData(const SkinningJob& _job) {
  const int influences_count = _job.influences_count;
  const size_t matrix_count = _job.joint_matrices.size();
  const int vertex_count = _job.vertex_count;

  const uint16_t* indices = _job.joint_indices.begin();
  const float* weights = _job.joint_weights.begin();
  const int explicit_weights_count = influences_count - 1;

  for (int v = 0; v < vertex_count; ++v) {
    // Checks every index, including those of zero-weight influences.
    for (int j = 0; j < influences_count; ++j) {
      const uint16_t index = indices[j];
      if (static_cast<size_t>(index) >= matrix_count) {
        return false;
      }
    }

    // Checks explicit weights and their sum.
    float weight_sum = 0.f;
    for (int j = 0; j < explicit_weights_count; ++j) {
      const float weight = weights[j];
      if (!std::isfinite(weight) || weight < 0.f || weight > 1.f) {
        return false;
      }
      weight_sum += weight;
    }
    if (!(weight_sum <= 1.f)) {
      return false;
    }

    // Checks the restored weight. It is implicitly 1 when there's a single
    // influence.
    if (explicit_weights_count > 0) {
      const float restored = 1.f - weight_sum;
      if (!std::isfinite(restored) || restored < 0.f || restored > 1.f) {
        return false;
      }
    }

    indices = reinterpret_cast<const uint16_t*>(
        reinterpret_cast<uintptr_t>(indices) + _job.joint_indices_stride);
    if (explicit_weights_count > 0) {
      weights = reinterpret_cast<const float*>(
          reinterpret_cast<uintptr_t>(weights) + _job.joint_weights_stride);
    }
  }

  return true;
}

// Applies the legacy linear blend skinning formula to the current vertex.
// _weight_at returns the weight of influence _j (restoring the last one).
template <typename _WeightAt>
void LinearFallback(const SkinningJob& _job, _WeightAt _weight_at,
                    const uint16_t* _indices, const float* _in_position,
                    const float* _in_normal, const float* _in_tangent,
                    float* _out_position, float* _out_normal,
                    float* _out_tangent) {
  const int influences_count = _job.influences_count;
  const bool has_normals = _in_normal != nullptr;
  const bool has_tangents = _in_tangent != nullptr;
  const bool has_inverse_transpose =
      !_job.joint_inverse_transpose_matrices.empty();

  // Accumulates the weighted matrix, exactly like the linear path does. The
  // accumulation order matches the legacy specializations (influence 0 first,
  // then 1, 2, ...) so that the fallback result is numerically identical to the
  // kLinear mode.
  float columns[4][4];
  float it_columns[4][4];
  for (int c = 0; c < 4; ++c) {
    for (int r = 0; r < 4; ++r) {
      columns[c][r] = 0.f;
      it_columns[c][r] = 0.f;
    }
  }
  for (int j = 0; j < influences_count; ++j) {
    const float weight = _weight_at(j);
    // A zero-weight influence contributes nothing to a finite matrix. It is
    // skipped so that a non finite matrix can't contaminate the result through
    // a 0 * NaN product.
    if (weight == 0.f) {
      continue;
    }
    const math::Float4x4& m = _job.joint_matrices[_indices[j]];
    for (int c = 0; c < 4; ++c) {
      columns[c][0] += MatrixComponent(m, c, 0) * weight;
      columns[c][1] += MatrixComponent(m, c, 1) * weight;
      columns[c][2] += MatrixComponent(m, c, 2) * weight;
      columns[c][3] += MatrixComponent(m, c, 3) * weight;
    }
    if (has_inverse_transpose) {
      const math::Float4x4& itm =
          _job.joint_inverse_transpose_matrices[_indices[j]];
      for (int c = 0; c < 4; ++c) {
        it_columns[c][0] += MatrixComponent(itm, c, 0) * weight;
        it_columns[c][1] += MatrixComponent(itm, c, 1) * weight;
        it_columns[c][2] += MatrixComponent(itm, c, 2) * weight;
        it_columns[c][3] += MatrixComponent(itm, c, 3) * weight;
      }
    }
  }
  if (!has_inverse_transpose) {
    for (int c = 0; c < 4; ++c) {
      for (int r = 0; r < 4; ++r) {
        it_columns[c][r] = columns[c][r];
      }
    }
  }

  // Position: full affine transform.
  _out_position[0] = columns[0][0] * _in_position[0] +
                     columns[1][0] * _in_position[1] +
                     columns[2][0] * _in_position[2] + columns[3][0];
  _out_position[1] = columns[0][1] * _in_position[0] +
                     columns[1][1] * _in_position[1] +
                     columns[2][1] * _in_position[2] + columns[3][1];
  _out_position[2] = columns[0][2] * _in_position[0] +
                     columns[1][2] * _in_position[1] +
                     columns[2][2] * _in_position[2] + columns[3][2];

  // Normal: vector transform, using inverse transpose matrices when available.
  if (has_normals) {
    _out_normal[0] = it_columns[0][0] * _in_normal[0] +
                     it_columns[1][0] * _in_normal[1] +
                     it_columns[2][0] * _in_normal[2];
    _out_normal[1] = it_columns[0][1] * _in_normal[0] +
                     it_columns[1][1] * _in_normal[1] +
                     it_columns[2][1] * _in_normal[2];
    _out_normal[2] = it_columns[0][2] * _in_normal[0] +
                     it_columns[1][2] * _in_normal[1] +
                     it_columns[2][2] * _in_normal[2];
  }
  if (has_tangents) {
    _out_tangent[0] = it_columns[0][0] * _in_tangent[0] +
                      it_columns[1][0] * _in_tangent[1] +
                      it_columns[2][0] * _in_tangent[2];
    _out_tangent[1] = it_columns[0][1] * _in_tangent[0] +
                      it_columns[1][1] * _in_tangent[1] +
                      it_columns[2][1] * _in_tangent[2];
    _out_tangent[2] = it_columns[0][2] * _in_tangent[0] +
                      it_columns[1][2] * _in_tangent[1] +
                      it_columns[2][2] * _in_tangent[2];
  }
}

// Skins the whole mesh using dual quaternion skinning, with a per-vertex
// linear blend fallback. Doesn't allocate memory.
void SkinningDualQuaternion(const SkinningJob& _job) {
  const int vertex_count = _job.vertex_count;
  const int influences_count = _job.influences_count;
  const int explicit_weights_count = influences_count - 1;
  const bool has_normals = !_job.in_normals.empty();
  const bool has_tangents = !_job.in_tangents.empty();

  const uint16_t* indices = _job.joint_indices.begin();
  const float* weights = _job.joint_weights.begin();
  const float* in_positions = _job.in_positions.begin();
  const float* in_normals =
      has_normals ? _job.in_normals.begin() : nullptr;
  const float* in_tangents =
      has_tangents ? _job.in_tangents.begin() : nullptr;
  float* out_positions = _job.out_positions.begin();
  float* out_normals = has_normals ? _job.out_normals.begin() : nullptr;
  float* out_tangents = has_tangents ? _job.out_tangents.begin() : nullptr;

  for (int v = 0; v < vertex_count; ++v) {
    // Reads all the inputs into local storage before writing any output, so
    // that input and output attributes can be fully aliased.
    float position[3] = {in_positions[0], in_positions[1], in_positions[2]};
    float normal[3] = {0.f, 0.f, 0.f};
    float tangent[3] = {0.f, 0.f, 0.f};
    if (has_normals) {
      normal[0] = in_normals[0];
      normal[1] = in_normals[1];
      normal[2] = in_normals[2];
    }
    if (has_tangents) {
      tangent[0] = in_tangents[0];
      tangent[1] = in_tangents[1];
      tangent[2] = in_tangents[2];
    }

    // Computes the restored last weight.
    float explicit_weight_sum = 0.f;
    for (int j = 0; j < explicit_weights_count; ++j) {
      explicit_weight_sum += weights[j];
    }
    const auto weight_at = [explicit_weights_count, explicit_weight_sum,
                            weights](int _j) {
      return _j < explicit_weights_count ? weights[_j]
                                        : 1.f - explicit_weight_sum;
    };

    // Selects the reference influence: the strictly positive weight with the
    // greatest value, ties being resolved by the smallest palette index.
    // The restored weight guarantees at least one strictly positive influence,
    // but the guard keeps the code safe if the data ever contradicts this.
    int reference = -1;
    float reference_weight = -1.f;
    for (int j = 0; j < influences_count; ++j) {
      const float weight = weight_at(j);
      if (weight <= 0.f) {
        continue;
      }
      const uint16_t index = indices[j];
      if (reference < 0 || weight > reference_weight ||
          (weight == reference_weight && index < indices[reference])) {
        reference = j;
        reference_weight = weight;
      }
    }

    // Extracts the reference rotation. If there's no positive influence or if
    // the rotation can't be built, the vertex is fully handled by linear blend
    // skinning.
    float ref_qx = 0.f, ref_qy = 0.f, ref_qz = 0.f, ref_qw = 1.f;
    bool use_dual_quaternion = false;
    if (reference >= 0) {
      const uint16_t reference_index = indices[reference];
      use_dual_quaternion =
          IsRigidMatrix(_job.joint_matrices[reference_index]) &&
          BuildRotation(_job.joint_matrices[reference_index], &ref_qx, &ref_qy,
                        &ref_qz, &ref_qw);
    }

    // Blends the dual quaternions, aligning every real part to the reference
    // one (same hemisphere). Zero-weight influences are skipped entirely.
    float rx = 0.f, ry = 0.f, rz = 0.f, rw = 0.f;
    float dx = 0.f, dy = 0.f, dz = 0.f, dw = 0.f;
    for (int j = 0; use_dual_quaternion && j < influences_count; ++j) {
      const float weight = weight_at(j);
      if (weight == 0.f) {
        continue;
      }
      const uint16_t index = indices[j];
      const math::Float4x4& m = _job.joint_matrices[index];
      if (!IsRigidMatrix(m)) {
        use_dual_quaternion = false;
        break;
      }
      float qx, qy, qz, qw;
      if (!BuildRotation(m, &qx, &qy, &qz, &qw)) {
        use_dual_quaternion = false;
        break;
      }

      // Dual part: 0.5 * (0, translation) * real.
      const float tx = MatrixComponent(m, 3, 0);
      const float ty = MatrixComponent(m, 3, 1);
      const float tz = MatrixComponent(m, 3, 2);
      const Quat tq = Multiply(tx, ty, tz, 0.f, qx, qy, qz, qw);
      float dqx = 0.5f * tq.x;
      float dqy = 0.5f * tq.y;
      float dqz = 0.5f * tq.z;
      float dqw = 0.5f * tq.w;

      // Aligns this influence rotation to the reference hemisphere. The dual
      // part is negated along with the real part so that the represented rigid
      // transformation is unchanged. A null dot product isn't flipped.
      const float dot =
          qx * ref_qx + qy * ref_qy + qz * ref_qz + qw * ref_qw;
      if (dot < 0.f) {
        qx = -qx;
        qy = -qy;
        qz = -qz;
        qw = -qw;
        dqx = -dqx;
        dqy = -dqy;
        dqz = -dqz;
        dqw = -dqw;
      }

      rx += weight * qx;
      ry += weight * qy;
      rz += weight * qz;
      rw += weight * qw;
      dx += weight * dqx;
      dy += weight * dqy;
      dz += weight * dqz;
      dw += weight * dqw;
    }

    float out_position[3];
    float out_normal[3];
    float out_tangent[3];

    if (use_dual_quaternion) {
      const float real_length =
          std::sqrt(rx * rx + ry * ry + rz * rz + rw * rw);
      if (real_length < kRealPartLengthTolerance) {
        use_dual_quaternion = false;
      } else {
        // Normalizes the real part and orthogonalizes the dual part.
        const float inv_length = 1.f / real_length;
        const float nx = rx * inv_length;
        const float ny = ry * inv_length;
        const float nz = rz * inv_length;
        const float nw = rw * inv_length;
        const float real_dot_dual =
            nx * dx + ny * dy + nz * dz + nw * dw;
        const float dualx = (dx - nx * real_dot_dual) * inv_length;
        const float dualy = (dy - ny * real_dot_dual) * inv_length;
        const float dualz = (dz - nz * real_dot_dual) * inv_length;
        const float dualw = (dw - nw * real_dot_dual) * inv_length;

        // Translation is 2 * (dual * conjugate(real)).
        const Quat t = Multiply(dualx, dualy, dualz, dualw, -nx, -ny, -nz, nw);
        const float tx = 2.f * t.x;
        const float ty = 2.f * t.y;
        const float tz = 2.f * t.z;

        float rotated[3];
        RotateVector(nx, ny, nz, nw, position[0], position[1], position[2],
                     &rotated[0], &rotated[1], &rotated[2]);
        out_position[0] = rotated[0] + tx;
        out_position[1] = rotated[1] + ty;
        out_position[2] = rotated[2] + tz;

        if (has_normals) {
          RotateVector(nx, ny, nz, nw, normal[0], normal[1], normal[2],
                       &out_normal[0], &out_normal[1], &out_normal[2]);
        }
        if (has_tangents) {
          RotateVector(nx, ny, nz, nw, tangent[0], tangent[1], tangent[2],
                       &out_tangent[0], &out_tangent[1], &out_tangent[2]);
        }
      }
    }

    if (!use_dual_quaternion) {
      LinearFallback(_job, weight_at, indices, position,
                     has_normals ? normal : nullptr,
                     has_tangents ? tangent : nullptr, out_position, out_normal,
                     out_tangent);
    }

    // Writes outputs, only their three float components.
    out_positions[0] = out_position[0];
    out_positions[1] = out_position[1];
    out_positions[2] = out_position[2];
    if (has_normals) {
      out_normals[0] = out_normal[0];
      out_normals[1] = out_normal[1];
      out_normals[2] = out_normal[2];
    }
    if (has_tangents) {
      out_tangents[0] = out_tangent[0];
      out_tangents[1] = out_tangent[1];
      out_tangents[2] = out_tangent[2];
    }

    // Strides to next vertex.
    indices = reinterpret_cast<const uint16_t*>(
        reinterpret_cast<uintptr_t>(indices) + _job.joint_indices_stride);
    if (explicit_weights_count > 0) {
      weights = reinterpret_cast<const float*>(
          reinterpret_cast<uintptr_t>(weights) + _job.joint_weights_stride);
    }
    in_positions = reinterpret_cast<const float*>(
        reinterpret_cast<uintptr_t>(in_positions) + _job.in_positions_stride);
    out_positions = reinterpret_cast<float*>(
        reinterpret_cast<uintptr_t>(out_positions) + _job.out_positions_stride);
    if (has_normals) {
      in_normals = reinterpret_cast<const float*>(
          reinterpret_cast<uintptr_t>(in_normals) + _job.in_normals_stride);
      out_normals = reinterpret_cast<float*>(
          reinterpret_cast<uintptr_t>(out_normals) + _job.out_normals_stride);
    }
    if (has_tangents) {
      in_tangents = reinterpret_cast<const float*>(
          reinterpret_cast<uintptr_t>(in_tangents) + _job.in_tangents_stride);
      out_tangents = reinterpret_cast<float*>(
          reinterpret_cast<uintptr_t>(out_tangents) +
          _job.out_tangents_stride);
    }
  }
}

}  // namespace

bool SkinningJob::Validate() const {
  // Start validation of all parameters.
  bool valid = true;

  // Checks mode is one of the known values.
  valid &= mode == kLinear || mode == kDualQuaternion;

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

  // Dual quaternion mode also validates all the data it reads. Runs only once
  // the span capacities are known to be valid, as it dereferences them.
  if (valid && mode == kDualQuaternion && vertex_count > 0) {
    valid &= ValidateDualQuaternionData(*this);
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

  // Dual quaternion skinning has its own implementation, which doesn't rely on
  // the specialized linear blend functions and their SIMD loads.
  if (mode == kDualQuaternion) {
    SkinningDualQuaternion(*this);
    return true;
  }

  // Find skinning function index.
  const size_t it = !joint_inverse_transpose_matrices.empty();
  assert(it < OZZ_ARRAY_SIZE(kSkinningFct));
  const size_t inf =
      static_cast<size_t>(influences_count) > OZZ_ARRAY_SIZE(kSkinningFct[0])
          ? OZZ_ARRAY_SIZE(kSkinningFct[0]) - 1
          : influences_count - 1;
  assert(inf < OZZ_ARRAY_SIZE(kSkinningFct[0]));
  const size_t fct = !in_normals.empty() + !in_tangents.empty();
  assert(fct < OZZ_ARRAY_SIZE(kSkinningFct[0][0]));

  // Calls skinning function. Cannot fail because job is valid.
  kSkinningFct[it][inf][fct](*this);

  return true;
}
}  // namespace geometry
}  // namespace ozz
