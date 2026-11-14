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

#ifndef OZZ_OZZ_ANIMATION_RUNTIME_POSE_PALETTE_CACHE_H_
#define OZZ_OZZ_ANIMATION_RUNTIME_POSE_PALETTE_CACHE_H_

#include <cstdint>

#include "ozz/animation/runtime/export.h"
#include "ozz/base/containers/vector.h"
#include "ozz/base/maths/simd_math.h"
#include "ozz/base/maths/transform.h"
#include "ozz/base/platform.h"
#include "ozz/base/span.h"

namespace ozz {

// Forward declaration math structures.
namespace math {
struct SoaTransform;
}

namespace animation {

// Forward declares the Skeleton object used to describe joint hierarchy.
class Skeleton;

// Caches local and model space joint matrices of a skeleton pose, so that
// skinning palettes can be built for any subset of joints without evaluating
// the whole hierarchy.
//
// The typical use case is a character whose body, equipment and low-detail
// skins are driven by the same skeleton, but use different (and partial)
// joint palettes. Instead of running a LocalToModelJob for the whole
// skeleton every time one of the skins is queried, a PosePaletteCache is
// prepared once for the skeleton, receives partial local-space pose updates,
// and lazily evaluates model-space matrices only along the ancestor chains
// of the requested joints. Cached matrices are reused across queries and
// across skins, as inverse-bind matrices are applied per output slot and
// never mixed into the cached model matrices.
//
// Usage sequence:
//  - Prepare() binds the cache to a skeleton and copies its rest pose. The
//    root matrix is initialized to identity and no model matrix is evaluated
//    yet. The skeleton must outlive the cache and must not be modified nor
//    moved while bound.
//  - UpdateLocals() copies the selected lanes of a complete SoA pose into
//    the cache. Only the listed joints are touched; their descendants are
//    invalidated implicitly and re-evaluated on the next query.
//  - SetRoot() replaces the character-to-world affine matrix. Bookkeeping
//    is constant time, no per-joint state is cleared.
//  - BuildPalette() evaluates the ancestor closure of the requested joints
//    (shared ancestors are evaluated once, untouched branches are left
//    unevaluated) and outputs, for every slot, the matrix
//    world(joint) * inverse_bind suitable for SkinningJob::joint_matrices.
//
// Prepare() is the only function that allocates memory. Every update and
// query is allocation free. Every instance is self contained, multiple
// caches can be interleaved safely. Functions are not thread safe though.
class OZZ_ANIMATION_DLL PosePaletteCache {
 public:
  // Counts the matrix operations actually performed by a call. Counters are
  // incremented at the exact place where the operation happens:
  // - local_matrices: local TRS to matrix conversions.
  // - model_matrices: parent (or root) by local matrix multiplications.
  // - palette_matrices: model by inverse-bind matrix multiplications.
  struct Stats {
    int local_matrices = 0;
    int model_matrices = 0;
    int palette_matrices = 0;
  };

  PosePaletteCache() = default;
  ~PosePaletteCache() = default;

  // Disables copy and assignation.
  PosePaletteCache(const PosePaletteCache&) = delete;
  PosePaletteCache& operator=(const PosePaletteCache&) = delete;

  // Binds the cache to _skeleton and copies its rest pose. The root matrix
  // is set to identity and model matrices are left unevaluated. Any
  // previously bound character is cleared. Allocates all the memory the
  // cache will ever need. Returns false if _skeleton is nullptr, in which
  // case the cache is left unbound.
  bool Prepare(const Skeleton* _skeleton);

  // Unbinds the skeleton and releases all memory. After Reset(), and until
  // Prepare() is called again, every update or query returns false.
  void Reset();

  // Returns true if the cache is bound to a skeleton.
  bool prepared() const { return skeleton_ != nullptr; }

  // Returns the number of joints of the bound skeleton, 0 if unbound.
  int num_joints() const { return num_joints_; }

  // Copies the selected lanes of a complete SoA pose into the cache.
  // _pose must store at least one SoaTransform per 4 joints of the skeleton
  // (ie skeleton->num_soa_joints()). _joints lists the joints to update, as
  // ascending, unique joint indices. Only those lanes are read, other lanes
  // (including padding lanes of the last SoA group) are never copied into
  // the cache, even if they contain non-finite values. The input can be
  // modified or released as soon as the function returns. Updated joints
  // invalidate their descendants lazily, no descendant is traversed here.
  // Returns false (without modifying the cache, nor _stats) if the cache is
  // not prepared, if _pose is too small, if an index is out of range,
  // duplicated or unordered, or if a selected lane holds a non-finite value
  // or a non-unit rotation (squared length further than 1e-3 from 1).
  bool UpdateLocals(span<const math::SoaTransform> _pose,
                    span<const int> _joints, Stats* _stats = nullptr);

  // Replaces the character-to-world affine matrix. Invalidation bookkeeping
  // is constant time. Returns false (without modifying the cache, nor
  // _stats) if the cache is not prepared or if _root is not a finite affine
  // matrix (last row must be 0, 0, 0, 1, within a 1e-4 tolerance).
  bool SetRoot(const math::Float4x4& _root, Stats* _stats = nullptr);

  // Outputs world(joint) * inverse_bind for every requested slot.
  // _joints can use any order and repeat joints. _inverse_binds and _output
  // must store at least _joints.size() elements. Inverse-bind matrices are
  // read per slot, they are never cached nor mixed into the model matrices,
  // so a single cache can serve any number of skins. Only the ancestor
  // closure of the requested joints is evaluated. _output must not overlap
  // _joints nor _inverse_binds.
  // Returns false (without modifying the cache, _output, nor _stats) if the
  // cache is not prepared, if a joint index is out of range, if
  // _inverse_binds or _output are too small, or if an inverse-bind matrix
  // is not a finite affine matrix.
  bool BuildPalette(span<const int> _joints,
                    span<const math::Float4x4> _inverse_binds,
                    span<math::Float4x4> _output, Stats* _stats = nullptr);

 private:
  // Per-joint lazy evaluation state. Generations are compared to detect
  // staleness without ever traversing descendants at write time.
  struct JointState {
    // Generation of the cached local transform, bumped by UpdateLocals().
    uint32_t local_gen = 0;
    // local_gen value the cached local matrix was built from.
    uint32_t local_matrix_gen = 0;
    // Version of the cached model matrix, bumped on every recomputation so
    // children can detect that their parent moved.
    uint32_t model_version = 0;
    // Parent's model_version the cached model matrix was built from.
    uint32_t model_parent_version = 0;
    // local_gen value the cached model matrix was built from.
    uint32_t model_local_gen = 0;
    // Root epoch the cached model matrix was built in.
    uint32_t model_epoch = 0;
  };

  // Ensures the model matrix of _joint (and of its ancestors) is up to
  // date, evaluating only the stale joints of the ancestor chain.
  void Ensure(int _joint, Stats* _stats);

  // Bound skeleton and its parent indices. Not owned.
  const Skeleton* skeleton_ = nullptr;
  span<const int16_t> parents_;
  int num_joints_ = 0;

  // Character-to-world affine matrix and its epoch. The epoch is bumped by
  // SetRoot() so that every cached model matrix is invalidated in constant
  // time.
  math::Float4x4 root_ = math::Float4x4::identity();
  uint32_t root_epoch_ = 0;

  // Global model matrix version counter.
  uint32_t version_counter_ = 0;

  // Cached local transforms (aos), local matrices and model matrices.
  ozz::vector<math::Transform> locals_;
  ozz::vector<math::Float4x4> local_matrices_;
  ozz::vector<math::Float4x4> model_matrices_;
  ozz::vector<JointState> states_;

  // Pre-allocated ancestor chain workspace used by Ensure().
  ozz::vector<int> chain_;
};
}  // namespace animation
}  // namespace ozz
#endif  // OZZ_OZZ_ANIMATION_RUNTIME_POSE_PALETTE_CACHE_H_
