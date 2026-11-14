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

#include "ozz/animation/runtime/pose_palette_cache.h"

#include <cmath>

#include "ozz/animation/runtime/skeleton.h"
#include "ozz/base/maths/soa_transform.h"

namespace ozz {
namespace animation {

namespace {

// Marks a generation that no valid state can ever hold.
const uint32_t kStaleGen = ~0u;

// Maximum distance between a rotation squared length and 1.
const float kUnitRotationTolerance = 1e-3f;

// Tolerance on the last row of an affine matrix: computed matrices (eg
// inverted bind poses) only approximate 0, 0, 0, 1.
const float kAffineRowTolerance = 1e-4f;

// Reads lane _lane of a SoA vector.
inline float SoaLane(math::_SimdFloat4 _v, int _lane) {
  alignas(16) float f[4];
  math::StorePtrU(_v, f);
  return f[_lane];
}

// Extracts the transform of lane _lane from a SoA transform.
math::Transform ExtractLane(const math::SoaTransform& _src, int _lane) {
  math::Transform dst;
  dst.translation.x = SoaLane(_src.translation.x, _lane);
  dst.translation.y = SoaLane(_src.translation.y, _lane);
  dst.translation.z = SoaLane(_src.translation.z, _lane);
  dst.rotation.x = SoaLane(_src.rotation.x, _lane);
  dst.rotation.y = SoaLane(_src.rotation.y, _lane);
  dst.rotation.z = SoaLane(_src.rotation.z, _lane);
  dst.rotation.w = SoaLane(_src.rotation.w, _lane);
  dst.scale.x = SoaLane(_src.scale.x, _lane);
  dst.scale.y = SoaLane(_src.scale.y, _lane);
  dst.scale.z = SoaLane(_src.scale.z, _lane);
  return dst;
}

// Tests whether all the components of a transform are finite, and its
// rotation is unit (within the squared length tolerance).
bool IsValid(const math::Transform& _t) {
  const math::Float3& t = _t.translation;
  const math::Quaternion& q = _t.rotation;
  const math::Float3& s = _t.scale;
  const bool finite = std::isfinite(t.x) && std::isfinite(t.y) &&
                      std::isfinite(t.z) && std::isfinite(q.x) &&
                      std::isfinite(q.y) && std::isfinite(q.z) &&
                      std::isfinite(q.w) && std::isfinite(s.x) &&
                      std::isfinite(s.y) && std::isfinite(s.z);
  const float len2 = q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w;
  return finite && std::fabs(len2 - 1.f) <= kUnitRotationTolerance;
}

// Tests whether _m is a finite affine matrix, ie all its components are
// finite and its last row is 0, 0, 0, 1 (within tolerance).
bool IsFiniteAffine(const math::Float4x4& _m) {
  for (int c = 0; c < 4; ++c) {
    alignas(16) float f[4];
    math::StorePtrU(_m.cols[c], f);
    for (int r = 0; r < 4; ++r) {
      if (!std::isfinite(f[r])) {
        return false;
      }
    }
  }
  return std::fabs(math::GetW(_m.cols[0])) <= kAffineRowTolerance &&
         std::fabs(math::GetW(_m.cols[1])) <= kAffineRowTolerance &&
         std::fabs(math::GetW(_m.cols[2])) <= kAffineRowTolerance &&
         std::fabs(math::GetW(_m.cols[3]) - 1.f) <= kAffineRowTolerance;
}

}  // namespace

bool PosePaletteCache::Prepare(const Skeleton* _skeleton) {
  // Clears any previously bound character.
  Reset();
  if (!_skeleton) {
    return false;
  }

  skeleton_ = _skeleton;
  parents_ = _skeleton->joint_parents();
  num_joints_ = _skeleton->num_joints();
  root_ = math::Float4x4::identity();
  root_epoch_ = 0;
  version_counter_ = 0;

  // Allocates all the memory the cache will ever need.
  locals_.resize(num_joints_);
  local_matrices_.resize(num_joints_);
  model_matrices_.resize(num_joints_);
  states_.resize(num_joints_);
  chain_.resize(num_joints_);

  // Copies the rest pose. Local and model matrices are left unevaluated.
  const span<const math::SoaTransform> rest = _skeleton->joint_rest_poses();
  for (int j = 0; j < num_joints_; ++j) {
    locals_[j] = ExtractLane(rest[j >> 2], j & 3);
    JointState& state = states_[j];
    state.local_gen = 0;
    state.local_matrix_gen = kStaleGen;
    state.model_version = 0;
    state.model_parent_version = 0;
    state.model_local_gen = 0;
    state.model_epoch = kStaleGen;
  }
  return true;
}

void PosePaletteCache::Reset() {
  skeleton_ = nullptr;
  parents_ = span<const int16_t>();
  num_joints_ = 0;
  root_ = math::Float4x4::identity();
  root_epoch_ = 0;
  version_counter_ = 0;

  // Releases all memory.
  ozz::vector<math::Transform>().swap(locals_);
  ozz::vector<math::Float4x4>().swap(local_matrices_);
  ozz::vector<math::Float4x4>().swap(model_matrices_);
  ozz::vector<JointState>().swap(states_);
  ozz::vector<int>().swap(chain_);
}

bool PosePaletteCache::UpdateLocals(span<const math::SoaTransform> _pose,
                                    span<const int> _joints, Stats* _stats) {
  if (!prepared()) {
    return false;
  }

  // The pose must be complete, even if only a subset of its lanes is used.
  if (_pose.size() < static_cast<size_t>((num_joints_ + 3) / 4)) {
    return false;
  }

  // Joint indices must be in range, ascending and unique.
  for (size_t i = 0; i < _joints.size(); ++i) {
    const int joint = _joints[i];
    if (joint < 0 || joint >= num_joints_ ||
        (i > 0 && joint <= _joints[i - 1])) {
      return false;
    }
  }

  // Validates every selected lane before touching the cache. Padding lanes
  // of the last SoA group are never selected, hence never validated.
  for (size_t i = 0; i < _joints.size(); ++i) {
    const int joint = _joints[i];
    if (!IsValid(ExtractLane(_pose[joint >> 2], joint & 3))) {
      return false;
    }
  }

  // Copies the selected lanes. Only those lanes are read from the input,
  // which can be modified or released as soon as the function returns.
  for (size_t i = 0; i < _joints.size(); ++i) {
    const int joint = _joints[i];
    locals_[joint] = ExtractLane(_pose[joint >> 2], joint & 3);
    // Bumping the local generation invalidates the joint and, lazily, its
    // descendants. No descendant is traversed here.
    ++states_[joint].local_gen;
  }

  if (_stats) {
    *_stats = Stats();
  }
  return true;
}

bool PosePaletteCache::SetRoot(const math::Float4x4& _root, Stats* _stats) {
  if (!prepared()) {
    return false;
  }
  if (!IsFiniteAffine(_root)) {
    return false;
  }

  root_ = _root;
  // Bumping the epoch invalidates every cached model matrix in constant
  // time, no per-joint state is cleared.
  ++root_epoch_;

  if (_stats) {
    *_stats = Stats();
  }
  return true;
}

bool PosePaletteCache::BuildPalette(span<const int> _joints,
                                    span<const math::Float4x4> _inverse_binds,
                                    span<math::Float4x4> _output,
                                    Stats* _stats) {
  if (!prepared()) {
    return false;
  }
  if (_inverse_binds.size() < _joints.size() ||
      _output.size() < _joints.size()) {
    return false;
  }

  // Validates every slot before evaluating anything, so that a failure
  // leaves the cache, the output and the stats untouched.
  for (size_t i = 0; i < _joints.size(); ++i) {
    if (_joints[i] < 0 || _joints[i] >= num_joints_) {
      return false;
    }
  }
  for (size_t i = 0; i < _joints.size(); ++i) {
    if (!IsFiniteAffine(_inverse_binds[i])) {
      return false;
    }
  }

  Stats stats;
  for (size_t i = 0; i < _joints.size(); ++i) {
    const int joint = _joints[i];
    Ensure(joint, &stats);
    // Inverse-bind matrices are applied per slot, they are never cached nor
    // mixed into the model matrices.
    _output[i] = model_matrices_[joint] * _inverse_binds[i];
    ++stats.palette_matrices;
  }

  if (_stats) {
    *_stats = stats;
  }
  return true;
}

void PosePaletteCache::Ensure(int _joint, Stats* _stats) {
  // Collects the ancestor chain, from _joint up to its root joint. Only the
  // ancestors of the requested joint are visited, the rest of the skeleton
  // (including unrequested sibling branches) is left untouched.
  int count = 0;
  for (int joint = _joint;;) {
    chain_[count++] = joint;
    const int parent = parents_[joint];
    if (parent == Skeleton::kNoParent) {
      break;
    }
    joint = parent;
  }

  // Evaluates the chain from the root down, so that parents are up to date
  // before their children.
  for (int i = count - 1; i >= 0; --i) {
    const int joint = chain_[i];
    JointState& state = states_[joint];

    // Rebuilds the local matrix only if the local transform changed since
    // the last conversion.
    if (state.local_matrix_gen != state.local_gen) {
      local_matrices_[joint] = math::Float4x4::FromAffine(locals_[joint]);
      state.local_matrix_gen = state.local_gen;
      ++_stats->local_matrices;
    }

    // Rebuilds the model matrix only if the root matrix, the local
    // transform or the parent model matrix changed since the last
    // multiplication.
    const int parent = parents_[joint];
    const bool stale =
        state.model_epoch != root_epoch_ ||
        state.model_local_gen != state.local_gen ||
        (parent != Skeleton::kNoParent &&
         state.model_parent_version != states_[parent].model_version);
    if (stale) {
      const math::Float4x4& parent_matrix =
          parent == Skeleton::kNoParent ? root_ : model_matrices_[parent];
      model_matrices_[joint] = parent_matrix * local_matrices_[joint];
      state.model_version = ++version_counter_;
      state.model_parent_version =
          parent == Skeleton::kNoParent ? 0 : states_[parent].model_version;
      state.model_local_gen = state.local_gen;
      state.model_epoch = root_epoch_;
      ++_stats->model_matrices;
    }
  }
}

}  // namespace animation
}  // namespace ozz
