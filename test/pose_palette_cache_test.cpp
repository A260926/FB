// PosePaletteCache task tests. Registers task_cases/task_case_count for the
// task driver (see task_support/check.h), no main() is defined here.

#include "task_support/check.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <vector>

#include "ozz/animation/offline/raw_skeleton.h"
#include "ozz/animation/offline/skeleton_builder.h"
#include "ozz/animation/runtime/local_to_model_job.h"
#include "ozz/animation/runtime/pose_palette_cache.h"
#include "ozz/animation/runtime/skeleton.h"
#include "ozz/base/maths/simd_math.h"
#include "ozz/base/maths/soa_transform.h"
#include "ozz/base/maths/transform.h"
#include "ozz/base/memory/allocator.h"
#include "ozz/base/memory/unique_ptr.h"
#include "ozz/geometry/runtime/skinning_job.h"

namespace {

using ozz::animation::PosePaletteCache;
using ozz::animation::Skeleton;
using ozz::math::Float3;
using ozz::math::Float4x4;
using ozz::math::Quaternion;
using ozz::math::SoaTransform;
using ozz::math::Transform;

// Deterministic PRNG (LCG), so that test cases are reproducible.
struct Rng {
  explicit Rng(uint32_t seed) : state(seed) {}
  float Next() {  // in [0, 1)
    state = state * 1664525u + 1013904223u;
    return static_cast<float>(state >> 8) * (1.f / 16777216.f);
  }
  float Range(float lo, float hi) { return lo + (hi - lo) * Next(); }
  uint32_t state;
};

Float3 SafeNormalize(const Float3& v) {
  const float len2 = v.x * v.x + v.y * v.y + v.z * v.z;
  if (len2 < 1e-8f) {
    return Float3(0.f, 1.f, 0.f);
  }
  const float inv = 1.f / std::sqrt(len2);
  return Float3(v.x * inv, v.y * inv, v.z * inv);
}

Quaternion RandomRotation(Rng& rng, float max_angle) {
  const Float3 axis = SafeNormalize(
      Float3(rng.Range(-1.f, 1.f), rng.Range(-1.f, 1.f), rng.Range(-1.f, 1.f)));
  return Quaternion::FromAxisAngle(axis, rng.Range(-max_angle, max_angle));
}

// Random transform with bounded, optionally negative, scale.
Transform RandomTransform(Rng& rng, float scale_lo, float scale_hi,
                          bool allow_negative, float max_angle = 2.5f) {
  Transform t;
  t.translation =
      Float3(rng.Range(-1.f, 1.f), rng.Range(-1.f, 1.f), rng.Range(-1.f, 1.f));
  t.rotation = RandomRotation(rng, max_angle);
  t.scale = Float3(rng.Range(scale_lo, scale_hi), rng.Range(scale_lo, scale_hi),
                   rng.Range(scale_lo, scale_hi));
  if (allow_negative) {
    if (rng.Next() < 0.25f) {
      t.scale.x = -t.scale.x;
    }
    if (rng.Next() < 0.25f) {
      t.scale.z = -t.scale.z;
    }
  }
  return t;
}

// A runtime skeleton plus its description in runtime (depth-first) order.
struct TestSkeleton {
  ozz::unique_ptr<Skeleton> skeleton;
  std::vector<int> parents;     // runtime order
  std::vector<Transform> rest;  // runtime order
  int num_joints() const { return static_cast<int>(parents.size()); }
  int num_soa() const { return (num_joints() + 3) / 4; }
};

void FillRawJoint(ozz::animation::offline::RawSkeleton::Joint& joint, int flat,
                  const std::vector<std::vector<int>>& children,
                  const std::vector<Transform>& locals) {
  char name[16];
  std::snprintf(name, sizeof(name), "j%d", flat);
  joint.name = name;
  joint.transform = locals[flat];
  joint.children.resize(children[flat].size());
  for (size_t c = 0; c < children[flat].size(); ++c) {
    FillRawJoint(joint.children[c], children[flat][c], children, locals);
  }
}

// Builds a runtime skeleton from a flat parent list (parents[i] < i, or -1
// for roots). Children are visited in ascending index order.
TestSkeleton MakeSkeleton(const std::vector<int>& flat_parents,
                          const std::vector<Transform>& flat_locals) {
  const int n = static_cast<int>(flat_parents.size());
  std::vector<std::vector<int>> children(n);
  std::vector<int> roots;
  for (int i = 0; i < n; ++i) {
    if (flat_parents[i] < 0) {
      roots.push_back(i);
    } else {
      children[flat_parents[i]].push_back(i);
    }
  }
  ozz::animation::offline::RawSkeleton raw;
  raw.roots.resize(roots.size());
  for (size_t r = 0; r < roots.size(); ++r) {
    FillRawJoint(raw.roots[r], roots[r], children, flat_locals);
  }
  TestSkeleton out;
  ozz::animation::offline::SkeletonBuilder builder;
  out.skeleton = builder(raw);
  // Maps runtime joints back to flat indices using joint names.
  const int nj = out.skeleton->num_joints();
  std::vector<int> flat_of(nj);
  for (int j = 0; j < nj; ++j) {
    flat_of[j] = std::atoi(out.skeleton->joint_names()[j] + 1);
  }
  out.parents.assign(out.skeleton->joint_parents().begin(),
                     out.skeleton->joint_parents().end());
  out.rest.resize(nj);
  for (int j = 0; j < nj; ++j) {
    out.rest[j] = flat_locals[flat_of[j]];
  }
  return out;
}

// 13 joints (not a multiple of 4), 2 roots, forked.
TestSkeleton MakeForkedSkeleton(uint32_t seed) {
  const std::vector<int> parents = {-1, 0, 1, 1, 3, -1, 5, 6, 6, 5, 9, 9, 8};
  Rng rng(seed);
  std::vector<Transform> rest(parents.size());
  for (size_t i = 0; i < rest.size(); ++i) {
    rest[i] = RandomTransform(rng, 0.8f, 1.4f, true);
  }
  return MakeSkeleton(parents, rest);
}

// Single chain of n joints.
TestSkeleton MakeChainSkeleton(uint32_t seed, int n) {
  std::vector<int> parents(n);
  std::vector<Transform> rest(n);
  Rng rng(seed);
  for (int i = 0; i < n; ++i) {
    parents[i] = i - 1;
    rest[i] = RandomTransform(rng, 0.8f, 1.3f, false);
  }
  return MakeSkeleton(parents, rest);
}

void SetSoaLane(SoaTransform& dst, int lane, const Transform& src) {
  alignas(16) float f[4];
#define OZZ_SET_LANE(member, value)          \
  ozz::math::StorePtrU(dst.member, f);       \
  f[lane] = value;                           \
  dst.member = ozz::math::simd_float4::LoadPtrU(f)
  OZZ_SET_LANE(translation.x, src.translation.x);
  OZZ_SET_LANE(translation.y, src.translation.y);
  OZZ_SET_LANE(translation.z, src.translation.z);
  OZZ_SET_LANE(rotation.x, src.rotation.x);
  OZZ_SET_LANE(rotation.y, src.rotation.y);
  OZZ_SET_LANE(rotation.z, src.rotation.z);
  OZZ_SET_LANE(rotation.w, src.rotation.w);
  OZZ_SET_LANE(scale.x, src.scale.x);
  OZZ_SET_LANE(scale.y, src.scale.y);
  OZZ_SET_LANE(scale.z, src.scale.z);
#undef OZZ_SET_LANE
}

// Sets all 10 components of one lane to the same value.
void SetLaneAll(SoaTransform& t, int lane, float v) {
  alignas(16) float f[4];
  ozz::math::SimdFloat4* members[10] = {&t.translation.x, &t.translation.y,
                                        &t.translation.z, &t.rotation.x,
                                        &t.rotation.y,    &t.rotation.z,
                                        &t.rotation.w,    &t.scale.x,
                                        &t.scale.y,       &t.scale.z};
  for (int m = 0; m < 10; ++m) {
    ozz::math::StorePtrU(*members[m], f);
    f[lane] = v;
    *members[m] = ozz::math::simd_float4::LoadPtrU(f);
  }
}

std::vector<SoaTransform> ToSoa(const std::vector<Transform>& pose) {
  std::vector<SoaTransform> soa((pose.size() + 3) / 4,
                                SoaTransform::identity());
  for (size_t j = 0; j < pose.size(); ++j) {
    SetSoaLane(soa[j >> 2], static_cast<int>(j) & 3, pose[j]);
  }
  return soa;
}

SoaTransform NanSoaTransform() {
  const float nan = std::numeric_limits<float>::quiet_NaN();
  SoaTransform t;
  SetLaneAll(t, 0, nan);
  SetLaneAll(t, 1, nan);
  SetLaneAll(t, 2, nan);
  SetLaneAll(t, 3, nan);
  return t;
}

// Fills the padding lanes (beyond num_joints) with NaN.
void FillPaddingWithNan(std::vector<SoaTransform>* soa, int num_joints) {
  const float nan = std::numeric_limits<float>::quiet_NaN();
  for (size_t j = num_joints; j < soa->size() * 4; ++j) {
    SetLaneAll((*soa)[j >> 2], static_cast<int>(j) & 3, nan);
  }
}

// Reference model matrices from the original LocalToModelJob.
bool RefModels(const Skeleton& skeleton, ozz::span<const SoaTransform> soa,
               const Float4x4* root, std::vector<Float4x4>* out) {
  out->resize(skeleton.num_joints());
  ozz::animation::LocalToModelJob job;
  job.skeleton = &skeleton;
  job.root = root;
  job.input = soa;
  job.output = ozz::make_span(*out);
  return job.Run();
}

bool RefFromExpected(const TestSkeleton& ts,
                     const std::vector<Transform>& expected,
                     const Float4x4* root, std::vector<Float4x4>* ref) {
  const std::vector<SoaTransform> soa = ToSoa(expected);
  return RefModels(*ts.skeleton, ozz::make_span(soa), root, ref);
}

// Compares matrices with a tolerance scaled by the magnitude of the
// reference.
bool MatrixNear(const Float4x4& a, const Float4x4& b, float tol) {
  float mag = 1.f;
  for (int c = 0; c < 4; ++c) {
    alignas(16) float fb[4];
    ozz::math::StorePtrU(b.cols[c], fb);
    for (int r = 0; r < 4; ++r) {
      mag = std::max(mag, std::fabs(fb[r]));
    }
  }
  for (int c = 0; c < 4; ++c) {
    alignas(16) float fa[4], fb[4];
    ozz::math::StorePtrU(a.cols[c], fa);
    ozz::math::StorePtrU(b.cols[c], fb);
    for (int r = 0; r < 4; ++r) {
      if (std::fabs(fa[r] - fb[r]) > tol * mag) {
        return false;
      }
    }
  }
  return true;
}

// Builds the palette with the cache and checks every slot against an
// independent per-slot multiplication of the reference model matrices.
int CheckPalette(PosePaletteCache& cache, const std::vector<int>& joints,
                 const std::vector<Float4x4>& binds,
                 const std::vector<Float4x4>& ref_models, float tol,
                 PosePaletteCache::Stats* stats) {
  std::vector<Float4x4> palette(joints.size());
  TASK_CHECK(cache.BuildPalette(ozz::make_span(joints), ozz::make_span(binds),
                                ozz::make_span(palette), stats));
  for (size_t s = 0; s < joints.size(); ++s) {
    const Float4x4 expected = ref_models[joints[s]] * binds[s];
    TASK_CHECK(MatrixNear(palette[s], expected, tol));
  }
  return 0;
}

// Generates new random transforms for _joints, applies them to the cache
// and to _expected (the merged reference pose). The update input carries
// NaN in every lane that isn't updated.
int ApplyUpdate(PosePaletteCache& cache, const TestSkeleton& ts,
                std::vector<Transform>* expected,
                const std::vector<int>& joints, Rng& rng,
                float scale_lo = 0.7f, float scale_hi = 1.5f,
                bool allow_negative = true) {
  std::vector<SoaTransform> input(ts.num_soa(), NanSoaTransform());
  for (size_t i = 0; i < joints.size(); ++i) {
    const int j = joints[i];
    const Transform t =
        RandomTransform(rng, scale_lo, scale_hi, allow_negative);
    (*expected)[j] = t;
    SetSoaLane(input[j >> 2], j & 3, t);
  }
  TASK_CHECK(cache.UpdateLocals(ozz::make_span(input),
                                ozz::make_span(joints), nullptr));
  return 0;
}

// Marks the ancestor closure (joints included) of _joints.
std::vector<char> AncestorClosure(const std::vector<int>& parents,
                                  const std::vector<int>& joints) {
  std::vector<char> marked(parents.size(), 0);
  for (size_t i = 0; i < joints.size(); ++i) {
    for (int k = joints[i]; k >= 0 && !marked[k]; k = parents[k]) {
      marked[k] = 1;
    }
  }
  return marked;
}

int CountMarked(const std::vector<char>& marked) {
  int count = 0;
  for (size_t i = 0; i < marked.size(); ++i) {
    count += marked[i];
  }
  return count;
}

// Counts the marked joints that are _u or one of its descendants.
int CountClosureBelow(const std::vector<int>& parents,
                      const std::vector<char>& marked, int u) {
  int count = 0;
  for (size_t j = 0; j < marked.size(); ++j) {
    if (!marked[j]) {
      continue;
    }
    for (int k = static_cast<int>(j);; k = parents[k]) {
      if (k == u) {
        ++count;
        break;
      }
      if (parents[k] < 0) {
        break;
      }
    }
  }
  return count;
}

int Depth(const std::vector<int>& parents, int j) {
  int d = 0;
  for (int k = j; parents[k] >= 0; k = parents[k]) {
    ++d;
  }
  return d;
}

std::vector<int> AllJoints(int n) {
  std::vector<int> joints(n);
  for (int i = 0; i < n; ++i) {
    joints[i] = i;
  }
  return joints;
}

// Inverse of the rest-pose model matrices: non-identity inverse binds.
std::vector<Float4x4> RestInverseBinds(const TestSkeleton& ts) {
  std::vector<Float4x4> rest_models;
  RefModels(*ts.skeleton, ts.skeleton->joint_rest_poses(), nullptr,
            &rest_models);
  std::vector<Float4x4> binds(rest_models.size());
  for (size_t j = 0; j < binds.size(); ++j) {
    binds[j] = ozz::math::Invert(rest_models[j]);
  }
  return binds;
}

// Counting allocator used to prove that updates and queries don't allocate.
class CountingAllocator : public ozz::memory::Allocator {
 public:
  explicit CountingAllocator(ozz::memory::Allocator* next) : next_(next) {}
  void* Allocate(size_t size, size_t alignment) override {
    ++allocs_;
    return next_->Allocate(size, alignment);
  }
  void Deallocate(void* block) override {
    if (block) {
      ++frees_;
    }
    next_->Deallocate(block);
  }
  int allocs_ = 0;
  int frees_ = 0;

 private:
  ozz::memory::Allocator* next_;
};

// ---------------------------------------------------------------------------
// values group
// ---------------------------------------------------------------------------

// Forked, multi-root, non-multiple-of-4 skeleton: full update, shuffled
// query with duplicates, non-identity binds, root with rotation and
// non-uniform scale.
int ForkedPalette() {
  TestSkeleton ts = MakeForkedSkeleton(7);
  TASK_CHECK(ts.num_joints() == 13);
  TASK_CHECK(ts.num_joints() % 4 != 0);

  PosePaletteCache cache;
  TASK_CHECK(cache.Prepare(ts.skeleton.get()));
  TASK_CHECK(cache.prepared());
  TASK_CHECK(cache.num_joints() == 13);

  // Full pose update. Padding lanes carry NaN and must be ignored.
  Rng rng(11);
  std::vector<Transform> pose(13);
  for (size_t i = 0; i < pose.size(); ++i) {
    pose[i] = RandomTransform(rng, 0.7f, 1.6f, true);
  }
  std::vector<SoaTransform> soa = ToSoa(pose);
  FillPaddingWithNan(&soa, 13);
  const std::vector<int> all = AllJoints(13);
  TASK_CHECK(cache.UpdateLocals(ozz::make_span(soa), ozz::make_span(all),
                                nullptr));

  // Root with rotation and non-uniform scale.
  const Float4x4 root = Float4x4::Translation(Float3(1.f, 2.f, 3.f)) *
                        Float4x4::FromAxisAngle(Float3(0.f, 1.f, 0.f), 0.7f) *
                        Float4x4::Scaling(Float3(2.f, 0.5f, 1.5f));
  TASK_CHECK(cache.SetRoot(root, nullptr));

  // Reference from the original LocalToModelJob.
  std::vector<Float4x4> ref;
  TASK_CHECK(RefModels(*ts.skeleton, ozz::make_span(soa), &root, &ref));

  // Arbitrary order, duplicates allowed.
  const std::vector<int> joints = {12, 0, 7, 7, 2, 11, 5, 12, 4, 9};
  const std::vector<Float4x4> rest_binds = RestInverseBinds(ts);
  std::vector<Float4x4> binds(joints.size());
  for (size_t s = 0; s < joints.size(); ++s) {
    binds[s] = rest_binds[joints[s]];
  }
  // Slot 1 gets a distinct non-identity bind: binds are per-slot.
  binds[1] = Float4x4::Translation(Float3(0.25f, -0.5f, 0.75f)) *
             Float4x4::Scaling(Float3(1.5f, 0.75f, 0.5f));

  PosePaletteCache::Stats stats;
  TASK_CHECK(CheckPalette(cache, joints, binds, ref, 2e-4f, &stats) == 0);
  // First query evaluates the ancestor closure of the requested joints.
  const int closure = CountMarked(AncestorClosure(ts.parents, joints));
  TASK_CHECK(stats.local_matrices == closure);
  TASK_CHECK(stats.model_matrices == closure);
  TASK_CHECK(stats.palette_matrices == static_cast<int>(joints.size()));

  // Empty update and empty query are legal.
  TASK_CHECK(
      cache.UpdateLocals(ozz::make_span(soa), ozz::span<const int>(), nullptr));
  TASK_CHECK(cache.BuildPalette(ozz::span<const int>(),
                                ozz::span<const Float4x4>(),
                                ozz::span<Float4x4>(), &stats));
  TASK_CHECK(stats.local_matrices == 0 && stats.model_matrices == 0 &&
             stats.palette_matrices == 0);
  return 0;
}

// Partial update: only the selected lanes are copied, every other lane of
// the input (real joints and padding) carries NaN.
int PartialLanes() {
  TestSkeleton ts = MakeForkedSkeleton(13);
  PosePaletteCache cache;
  TASK_CHECK(cache.Prepare(ts.skeleton.get()));

  Rng rng(17);
  std::vector<Transform> expected = ts.rest;
  TASK_CHECK(ApplyUpdate(cache, ts, &expected, {1, 6, 9, 12}, rng) == 0);

  // The whole palette must match the merged pose: unupdated joints keep
  // their rest pose, NaN lanes never entered the cache.
  std::vector<Float4x4> ref;
  TASK_CHECK(RefFromExpected(ts, expected, nullptr, &ref));
  const std::vector<int> all = AllJoints(13);
  const std::vector<Float4x4> binds = RestInverseBinds(ts);
  PosePaletteCache::Stats stats;
  TASK_CHECK(CheckPalette(cache, all, binds, ref, 2e-4f, &stats) == 0);
  // Only the 4 updated joints needed a local conversion; models were
  // evaluated for the whole closure.
  TASK_CHECK(stats.local_matrices == 13);
  TASK_CHECK(stats.model_matrices == 13);
  return 0;
}

// One cache serves two skins with different binds and different joint
// subsets; model matrices are shared, binds are never mixed in.
int TwoSkins() {
  TestSkeleton ts = MakeForkedSkeleton(19);
  PosePaletteCache cache;
  TASK_CHECK(cache.Prepare(ts.skeleton.get()));

  Rng rng(23);
  std::vector<Transform> expected = ts.rest;
  TASK_CHECK(ApplyUpdate(cache, ts, &expected, {0, 3, 6, 10}, rng) == 0);
  std::vector<Float4x4> ref;
  TASK_CHECK(RefFromExpected(ts, expected, nullptr, &ref));

  // Skin A: all joints, binds from the rest pose.
  const std::vector<int> all = AllJoints(13);
  const std::vector<Float4x4> binds_a = RestInverseBinds(ts);
  PosePaletteCache::Stats stats;
  std::vector<Float4x4> palette_a(13);
  TASK_CHECK(cache.BuildPalette(ozz::make_span(all), ozz::make_span(binds_a),
                                ozz::make_span(palette_a), &stats));
  TASK_CHECK(stats.local_matrices == 13 && stats.model_matrices == 13);
  for (int j = 0; j < 13; ++j) {
    TASK_CHECK(MatrixNear(palette_a[j], ref[j] * binds_a[j], 2e-4f));
  }

  // Skin B: different subset, different order, different binds (inverse of
  // the animated pose models).
  const std::vector<int> joints_b = {12, 4, 10, 2, 0};
  std::vector<Float4x4> binds_b(joints_b.size());
  for (size_t s = 0; s < joints_b.size(); ++s) {
    binds_b[s] = ozz::math::Invert(ref[joints_b[s]]);
  }
  std::vector<Float4x4> palette_b(joints_b.size());
  TASK_CHECK(cache.BuildPalette(ozz::make_span(joints_b),
                                ozz::make_span(binds_b),
                                ozz::make_span(palette_b), &stats));
  // Everything was already evaluated by skin A.
  TASK_CHECK(stats.local_matrices == 0 && stats.model_matrices == 0);
  TASK_CHECK(stats.palette_matrices == static_cast<int>(joints_b.size()));
  for (size_t s = 0; s < joints_b.size(); ++s) {
    TASK_CHECK(MatrixNear(palette_b[s], ref[joints_b[s]] * binds_b[s], 2e-4f));
  }

  // Skin A again: identical result, skin B binds never leaked in.
  std::vector<Float4x4> palette_a2(13);
  TASK_CHECK(cache.BuildPalette(ozz::make_span(all), ozz::make_span(binds_a),
                                ozz::make_span(palette_a2), &stats));
  TASK_CHECK(stats.local_matrices == 0 && stats.model_matrices == 0);
  for (int j = 0; j < 13; ++j) {
    TASK_CHECK(MatrixNear(palette_a2[j], palette_a[j], 0.f));
  }
  return 0;
}

// Feeds a cache-built palette (non-identity binds) into a real SkinningJob
// and checks the skinned vertices.
int SkinningVertices() {
  TestSkeleton ts = MakeForkedSkeleton(29);
  PosePaletteCache cache;
  TASK_CHECK(cache.Prepare(ts.skeleton.get()));

  Rng rng(31);
  std::vector<Transform> expected = ts.rest;
  const std::vector<int> all = AllJoints(13);
  std::vector<SoaTransform> input(ts.num_soa(), NanSoaTransform());
  for (int j = 0; j < 13; ++j) {
    const Transform t = RandomTransform(rng, 0.7f, 1.5f, true);
    expected[j] = t;
    SetSoaLane(input[j >> 2], j & 3, t);
  }
  TASK_CHECK(
      cache.UpdateLocals(ozz::make_span(input), ozz::make_span(all), nullptr));

  // Palette with non-identity binds.
  const std::vector<Float4x4> binds = RestInverseBinds(ts);
  std::vector<Float4x4> palette(13);
  PosePaletteCache::Stats stats;
  TASK_CHECK(cache.BuildPalette(ozz::make_span(all), ozz::make_span(binds),
                                ozz::make_span(palette), &stats));

  // Independent reference palette.
  std::vector<Float4x4> ref;
  TASK_CHECK(RefFromExpected(ts, expected, nullptr, &ref));
  std::vector<Float4x4> ref_palette(13);
  for (int j = 0; j < 13; ++j) {
    ref_palette[j] = ref[j] * binds[j];
    TASK_CHECK(MatrixNear(palette[j], ref_palette[j], 2e-4f));
  }

  // 8 vertices, 2 influences each.
  const int nv = 8;
  std::vector<float> positions(3 * nv);
  std::vector<uint16_t> indices(2 * nv);
  std::vector<float> weights(nv);
  for (int v = 0; v < nv; ++v) {
    positions[3 * v + 0] = rng.Range(-2.f, 2.f);
    positions[3 * v + 1] = rng.Range(-2.f, 2.f);
    positions[3 * v + 2] = rng.Range(-2.f, 2.f);
    indices[2 * v + 0] = static_cast<uint16_t>(v % 13);
    indices[2 * v + 1] = static_cast<uint16_t>((2 * v + 5) % 13);
    weights[v] = 0.2f + 0.6f * (static_cast<float>(v) / (nv - 1));
  }
  std::vector<float> out_positions(3 * nv, -777.f);
  ozz::geometry::SkinningJob job;
  job.vertex_count = nv;
  job.influences_count = 2;
  job.joint_matrices = ozz::make_span(palette);
  job.joint_indices = ozz::make_span(indices);
  job.joint_indices_stride = 2 * sizeof(uint16_t);
  job.joint_weights = ozz::make_span(weights);
  job.joint_weights_stride = sizeof(float);
  job.in_positions = ozz::make_span(positions);
  job.in_positions_stride = 3 * sizeof(float);
  job.out_positions = ozz::make_span(out_positions);
  job.out_positions_stride = 3 * sizeof(float);
  TASK_CHECK(job.Run());

  // Independent per-vertex reference.
  for (int v = 0; v < nv; ++v) {
    const float* p = &positions[3 * v];
    float expected_pos[3] = {0.f, 0.f, 0.f};
    for (int influence = 0; influence < 2; ++influence) {
      const Float4x4& m = ref_palette[indices[2 * v + influence]];
      alignas(16) float c[4][4];
      for (int col = 0; col < 4; ++col) {
        ozz::math::StorePtrU(m.cols[col], c[col]);
      }
      const float w = influence == 0 ? weights[v] : 1.f - weights[v];
      for (int r = 0; r < 3; ++r) {
        expected_pos[r] += w * (c[0][r] * p[0] + c[1][r] * p[1] +
                                c[2][r] * p[2] + c[3][r]);
      }
    }
    for (int r = 0; r < 3; ++r) {
      const float tol = 1e-4f * std::max(1.f, std::fabs(expected_pos[r]));
      TASK_CHECK(std::fabs(out_positions[3 * v + r] - expected_pos[r]) <= tol);
    }
  }
  return 0;
}

// The update input can be released as soon as UpdateLocals returns.
int InputFreed() {
  TestSkeleton ts = MakeForkedSkeleton(37);
  PosePaletteCache cache;
  TASK_CHECK(cache.Prepare(ts.skeleton.get()));

  Rng rng(41);
  std::vector<Transform> expected = ts.rest;
  const std::vector<int> subset = {2, 5, 8};
  {
    std::vector<SoaTransform>* input =
        new std::vector<SoaTransform>(ts.num_soa(), NanSoaTransform());
    for (size_t i = 0; i < subset.size(); ++i) {
      const Transform t = RandomTransform(rng, 0.7f, 1.5f, true);
      expected[subset[i]] = t;
      SetSoaLane((*input)[subset[i] >> 2], subset[i] & 3, t);
    }
    TASK_CHECK(cache.UpdateLocals(ozz::make_span(*input),
                                  ozz::make_span(subset), nullptr));
    delete input;  // Input released right after the call.
  }

  // Query after the input was freed.
  std::vector<Float4x4> ref;
  TASK_CHECK(RefFromExpected(ts, expected, nullptr, &ref));
  const std::vector<int> joints = {8, 2, 5, 0, 12};
  std::vector<Float4x4> binds(joints.size(), Float4x4::identity());
  PosePaletteCache::Stats stats;
  TASK_CHECK(CheckPalette(cache, joints, binds, ref, 2e-4f, &stats) == 0);
  return 0;
}

// Non-uniform, negative and zero scales, in the pose and in the root.
int ExtremeScales() {
  TestSkeleton ts = MakeForkedSkeleton(43);
  PosePaletteCache cache;
  TASK_CHECK(cache.Prepare(ts.skeleton.get()));

  Rng rng(47);
  std::vector<Transform> pose(13);
  for (size_t i = 0; i < pose.size(); ++i) {
    pose[i] = RandomTransform(rng, 0.6f, 1.8f, true);
  }
  pose[3].scale = Float3(-1.5f, 0.5f, -2.f);  // negative scales
  pose[8].scale.y = 0.f;                       // zero scale mid-chain
  pose[12].scale = Float3(0.f, 0.f, 0.f);      // zero scale leaf
  std::vector<SoaTransform> soa = ToSoa(pose);
  const std::vector<int> all = AllJoints(13);
  TASK_CHECK(cache.UpdateLocals(ozz::make_span(soa), ozz::make_span(all),
                                nullptr));

  const Float4x4 root = Float4x4::Translation(Float3(-2.f, 1.f, 0.5f)) *
                        Float4x4::FromAxisAngle(Float3(1.f, 0.f, 0.f), -0.4f) *
                        Float4x4::Scaling(Float3(3.f, 0.25f, 1.25f));
  TASK_CHECK(cache.SetRoot(root, nullptr));

  std::vector<Float4x4> ref;
  TASK_CHECK(RefModels(*ts.skeleton, ozz::make_span(soa), &root, &ref));
  const std::vector<Float4x4> binds = RestInverseBinds(ts);
  PosePaletteCache::Stats stats;
  TASK_CHECK(CheckPalette(cache, all, binds, ref, 2e-4f, &stats) == 0);
  return 0;
}

// Multiple caches interleaved: states are independent.
int InterleavedCaches() {
  TestSkeleton a = MakeForkedSkeleton(53);
  TestSkeleton b = MakeChainSkeleton(59, 7);
  PosePaletteCache ca, cb;
  TASK_CHECK(ca.Prepare(a.skeleton.get()));
  TASK_CHECK(cb.Prepare(b.skeleton.get()));

  Rng rng(61);
  std::vector<Transform> expected_a = a.rest;
  std::vector<Transform> expected_b = b.rest;
  TASK_CHECK(ApplyUpdate(ca, a, &expected_a, {0, 5, 9}, rng) == 0);
  TASK_CHECK(ApplyUpdate(cb, b, &expected_b, {1, 4}, rng) == 0);

  std::vector<Float4x4> ref_a, ref_b;
  TASK_CHECK(RefFromExpected(a, expected_a, nullptr, &ref_a));
  TASK_CHECK(RefFromExpected(b, expected_b, nullptr, &ref_b));

  const std::vector<int> joints_a = {0, 5, 9, 12};
  const std::vector<int> joints_b = {0, 1, 4, 6};
  const std::vector<Float4x4> binds_a(joints_a.size(),
                                      Float4x4::identity());
  const std::vector<Float4x4> binds_b(joints_b.size(),
                                      Float4x4::identity());
  PosePaletteCache::Stats sa, sb;
  TASK_CHECK(CheckPalette(ca, joints_a, binds_a, ref_a, 2e-4f, &sa) == 0);
  TASK_CHECK(CheckPalette(cb, joints_b, binds_b, ref_b, 2e-4f, &sb) == 0);

  // Updating A must not affect B.
  TASK_CHECK(ApplyUpdate(ca, a, &expected_a, {2, 3}, rng) == 0);
  TASK_CHECK(CheckPalette(cb, joints_b, binds_b, ref_b, 2e-4f, &sb) == 0);
  TASK_CHECK(sb.local_matrices == 0 && sb.model_matrices == 0);

  TASK_CHECK(RefFromExpected(a, expected_a, nullptr, &ref_a));
  TASK_CHECK(CheckPalette(ca, joints_a, binds_a, ref_a, 2e-4f, &sa) == 0);
  return 0;
}

// ---------------------------------------------------------------------------
// invalidation group
// ---------------------------------------------------------------------------

// SetRoot invalidates model matrices in constant time; local matrices are
// reused.
int RootChange() {
  TestSkeleton ts = MakeForkedSkeleton(67);
  PosePaletteCache cache;
  TASK_CHECK(cache.Prepare(ts.skeleton.get()));

  Rng rng(71);
  std::vector<Transform> expected = ts.rest;
  const std::vector<int> all = AllJoints(13);
  TASK_CHECK(ApplyUpdate(cache, ts, &expected, all, rng) == 0);
  const std::vector<Float4x4> binds = RestInverseBinds(ts);

  std::vector<Float4x4> ref;
  TASK_CHECK(RefFromExpected(ts, expected, nullptr, &ref));
  PosePaletteCache::Stats stats;
  TASK_CHECK(CheckPalette(cache, all, binds, ref, 2e-4f, &stats) == 0);
  TASK_CHECK(stats.local_matrices == 13 && stats.model_matrices == 13);

  // New root: rotation and non-uniform scale.
  const Float4x4 root = Float4x4::Translation(Float3(1.f, -2.f, 0.5f)) *
                        Float4x4::FromAxisAngle(Float3(0.f, 0.f, 1.f), 1.1f) *
                        Float4x4::Scaling(Float3(1.5f, 2.5f, 0.5f));
  TASK_CHECK(cache.SetRoot(root, &stats));
  TASK_CHECK(stats.local_matrices == 0 && stats.model_matrices == 0 &&
             stats.palette_matrices == 0);

  std::vector<Float4x4> ref_rooted;
  TASK_CHECK(RefFromExpected(ts, expected, &root, &ref_rooted));
  TASK_CHECK(CheckPalette(cache, all, binds, ref_rooted, 2e-4f, &stats) == 0);
  // Models re-evaluated, locals reused.
  TASK_CHECK(stats.local_matrices == 0);
  TASK_CHECK(stats.model_matrices == 13);

  // Back to identity.
  TASK_CHECK(cache.SetRoot(Float4x4::identity(), nullptr));
  TASK_CHECK(CheckPalette(cache, all, binds, ref, 2e-4f, &stats) == 0);
  TASK_CHECK(stats.local_matrices == 0 && stats.model_matrices == 13);

  // A root change lazily invalidates partial chains too: querying a single
  // leaf only re-evaluates its own ancestor chain.
  TASK_CHECK(cache.SetRoot(root, nullptr));
  const std::vector<int> leaf = {4};
  const std::vector<Float4x4> bind4 = {binds[4]};
  TASK_CHECK(CheckPalette(cache, leaf, bind4, ref_rooted, 2e-4f, &stats) ==
             0);
  TASK_CHECK(stats.local_matrices == 0 && stats.model_matrices == 4);
  return 0;
}

// Ancestor changes recompute the chain layer by layer; sibling changes
// don't touch it; parent+child can be written together.
int AncestorAndSibling() {
  TestSkeleton ts = MakeForkedSkeleton(73);
  PosePaletteCache cache;
  TASK_CHECK(cache.Prepare(ts.skeleton.get()));

  Rng rng(79);
  std::vector<Transform> expected = ts.rest;
  const std::vector<int> all = AllJoints(13);
  TASK_CHECK(ApplyUpdate(cache, ts, &expected, all, rng) == 0);

  // Queries target leaf 4, whose chain is 4 -> 3 -> 1 -> 0.
  const std::vector<int> leaf = {4};
  const std::vector<Float4x4> rest_binds = RestInverseBinds(ts);
  const std::vector<Float4x4> binds = {rest_binds[4]};
  std::vector<Float4x4> ref;
  PosePaletteCache::Stats stats;

  TASK_CHECK(RefFromExpected(ts, expected, nullptr, &ref));
  TASK_CHECK(CheckPalette(cache, leaf, binds, ref, 2e-4f, &stats) == 0);
  TASK_CHECK(stats.local_matrices == 4 && stats.model_matrices == 4);

  // Warm query: nothing to do.
  TASK_CHECK(CheckPalette(cache, leaf, binds, ref, 2e-4f, &stats) == 0);
  TASK_CHECK(stats.local_matrices == 0 && stats.model_matrices == 0);

  // Ancestor update: joint 1. Joints 1, 3, 4 are re-evaluated.
  TASK_CHECK(ApplyUpdate(cache, ts, &expected, {1}, rng) == 0);
  TASK_CHECK(RefFromExpected(ts, expected, nullptr, &ref));
  TASK_CHECK(CheckPalette(cache, leaf, binds, ref, 2e-4f, &stats) == 0);
  TASK_CHECK(stats.local_matrices == 1 && stats.model_matrices == 3);

  // Sibling branch update: joint 12 doesn't touch the chain.
  TASK_CHECK(ApplyUpdate(cache, ts, &expected, {12}, rng) == 0);
  TASK_CHECK(CheckPalette(cache, leaf, binds, ref, 2e-4f, &stats) == 0);
  TASK_CHECK(stats.local_matrices == 0 && stats.model_matrices == 0);

  // Parent and child written together: joints 1 and 3.
  TASK_CHECK(ApplyUpdate(cache, ts, &expected, {1, 3}, rng) == 0);
  TASK_CHECK(RefFromExpected(ts, expected, nullptr, &ref));
  TASK_CHECK(CheckPalette(cache, leaf, binds, ref, 2e-4f, &stats) == 0);
  TASK_CHECK(stats.local_matrices == 2 && stats.model_matrices == 3);

  // Leaf itself.
  TASK_CHECK(ApplyUpdate(cache, ts, &expected, {4}, rng) == 0);
  TASK_CHECK(RefFromExpected(ts, expected, nullptr, &ref));
  TASK_CHECK(CheckPalette(cache, leaf, binds, ref, 2e-4f, &stats) == 0);
  TASK_CHECK(stats.local_matrices == 1 && stats.model_matrices == 1);

  // Evaluating a moved ancestor first doesn't settle the descendants: the
  // next query of the leaf recomputes the chain through the new parent
  // version.
  TASK_CHECK(ApplyUpdate(cache, ts, &expected, {1}, rng) == 0);
  TASK_CHECK(RefFromExpected(ts, expected, nullptr, &ref));
  const std::vector<int> mid = {1};
  const std::vector<Float4x4> binds_mid = {rest_binds[1]};
  TASK_CHECK(CheckPalette(cache, mid, binds_mid, ref, 2e-4f, &stats) == 0);
  TASK_CHECK(stats.local_matrices == 1 && stats.model_matrices == 1);
  TASK_CHECK(CheckPalette(cache, leaf, binds, ref, 2e-4f, &stats) == 0);
  TASK_CHECK(stats.local_matrices == 0 && stats.model_matrices == 2);
  return 0;
}

// Every failure leaves the cache, the output and the stats untouched, and
// valid calls keep working afterwards.
int FailedCalls() {
  TestSkeleton ts = MakeForkedSkeleton(83);
  Rng rng(89);
  PosePaletteCache cache;

  std::vector<Transform> pose(13);
  for (size_t i = 0; i < pose.size(); ++i) {
    pose[i] = RandomTransform(rng, 0.7f, 1.5f, true);
  }
  const std::vector<SoaTransform> soa = ToSoa(pose);
  const std::vector<int> all = AllJoints(13);
  const Float4x4 identity = Float4x4::identity();
  std::vector<Float4x4> binds(13, identity);
  std::vector<Float4x4> out(13);

  // Unprepared cache: everything fails.
  TASK_CHECK(!cache.UpdateLocals(ozz::make_span(soa), ozz::make_span(all),
                                 nullptr));
  TASK_CHECK(!cache.SetRoot(identity, nullptr));
  TASK_CHECK(!cache.BuildPalette(ozz::make_span(all), ozz::make_span(binds),
                                 ozz::make_span(out), nullptr));

  TASK_CHECK(cache.Prepare(ts.skeleton.get()));
  TASK_CHECK(cache.UpdateLocals(ozz::make_span(soa), ozz::make_span(all),
                                nullptr));
  std::vector<Float4x4> ref;
  TASK_CHECK(RefModels(*ts.skeleton, ozz::make_span(soa), nullptr, &ref));
  PosePaletteCache::Stats stats;
  TASK_CHECK(CheckPalette(cache, all, binds, ref, 2e-4f, &stats) == 0);

  // Sentinel stats and output that failed calls must not modify.
  PosePaletteCache::Stats sentinel;
  sentinel.local_matrices = 777;
  sentinel.model_matrices = 888;
  sentinel.palette_matrices = 999;
  const Float4x4 sentinel_matrix =
      Float4x4::Scaling(Float3(7.f, 8.f, 9.f));

#define TASK_CHECK_UNTOUCHED(s)                       \
  TASK_CHECK(s.local_matrices == 777 &&               \
             s.model_matrices == 888 &&               \
             s.palette_matrices == 999)

  // Duplicate and unordered update indices.
  PosePaletteCache::Stats s = sentinel;
  std::vector<int> bad = {2, 2};
  TASK_CHECK(
      !cache.UpdateLocals(ozz::make_span(soa), ozz::make_span(bad), &s));
  TASK_CHECK_UNTOUCHED(s);
  bad = {5, 3};
  s = sentinel;
  TASK_CHECK(
      !cache.UpdateLocals(ozz::make_span(soa), ozz::make_span(bad), &s));
  TASK_CHECK_UNTOUCHED(s);

  // Out-of-range update indices.
  bad = {13};
  s = sentinel;
  TASK_CHECK(
      !cache.UpdateLocals(ozz::make_span(soa), ozz::make_span(bad), &s));
  TASK_CHECK_UNTOUCHED(s);
  bad = {-1};
  s = sentinel;
  TASK_CHECK(
      !cache.UpdateLocals(ozz::make_span(soa), ozz::make_span(bad), &s));
  TASK_CHECK_UNTOUCHED(s);

  // Pose too small.
  s = sentinel;
  TASK_CHECK(!cache.UpdateLocals(
      ozz::span<const SoaTransform>(soa.data(), soa.size() - 1),
      ozz::make_span(all), &s));
  TASK_CHECK_UNTOUCHED(s);

  // Non-finite selected lane.
  {
    std::vector<SoaTransform> dirty = soa;
    const float nan = std::numeric_limits<float>::quiet_NaN();
    alignas(16) float f[4];
    ozz::math::StorePtrU(dirty[0].translation.x, f);
    f[2] = nan;  // joint 2
    dirty[0].translation.x = ozz::math::simd_float4::LoadPtrU(f);
    bad = {2};
    s = sentinel;
    TASK_CHECK(
        !cache.UpdateLocals(ozz::make_span(dirty), ozz::make_span(bad), &s));
    TASK_CHECK_UNTOUCHED(s);
  }

  // Non-unit rotation in a selected lane.
  {
    std::vector<SoaTransform> dirty = soa;
    Transform t = pose[2];
    t.rotation = Quaternion(0.f, 0.f, 0.f, 2.f);
    SetSoaLane(dirty[0], 2, t);
    bad = {2};
    s = sentinel;
    TASK_CHECK(
        !cache.UpdateLocals(ozz::make_span(dirty), ozz::make_span(bad), &s));
    TASK_CHECK_UNTOUCHED(s);
  }

  // NaN in a lane that isn't selected: not a failure, and the cache keeps
  // its previous value for that joint.
  {
    std::vector<SoaTransform> dirty = soa;
    SetLaneAll(dirty[1], 1, std::numeric_limits<float>::quiet_NaN());
    bad = {2};
    s = sentinel;
    TASK_CHECK(
        cache.UpdateLocals(ozz::make_span(dirty), ozz::make_span(bad), &s));
    TASK_CHECK(s.local_matrices == 0 && s.model_matrices == 0 &&
               s.palette_matrices == 0);
    TASK_CHECK(CheckPalette(cache, all, binds, ref, 2e-4f, &stats) == 0);
  }

  // Non-finite and non-affine root matrices.
  {
    Float4x4 m = identity;
    const float nan = std::numeric_limits<float>::quiet_NaN();
    m.cols[0] = ozz::math::simd_float4::Load(nan, 0.f, 0.f, 0.f);
    s = sentinel;
    TASK_CHECK(!cache.SetRoot(m, &s));
    TASK_CHECK_UNTOUCHED(s);
    m = identity;
    m.cols[3] = ozz::math::simd_float4::Load(0.f, 0.f, 0.f, 2.f);
    s = sentinel;
    TASK_CHECK(!cache.SetRoot(m, &s));
    TASK_CHECK_UNTOUCHED(s);
  }

  // Out-of-range palette joints; output and stats untouched.
  {
    bad = {0, 13};
    s = sentinel;
    for (size_t i = 0; i < out.size(); ++i) {
      out[i] = sentinel_matrix;
    }
    TASK_CHECK(!cache.BuildPalette(ozz::make_span(bad), ozz::make_span(binds),
                                   ozz::make_span(out), &s));
    TASK_CHECK_UNTOUCHED(s);
    TASK_CHECK(MatrixNear(out[0], sentinel_matrix, 0.f));
    bad = {-1};
    s = sentinel;
    TASK_CHECK(!cache.BuildPalette(ozz::make_span(bad), ozz::make_span(binds),
                                   ozz::make_span(out), &s));
    TASK_CHECK_UNTOUCHED(s);
  }

  // Non-finite and non-affine inverse binds.
  {
    std::vector<Float4x4> bad_binds = binds;
    const float nan = std::numeric_limits<float>::quiet_NaN();
    bad_binds[3].cols[2] = ozz::math::simd_float4::Load(0.f, nan, 0.f, 0.f);
    bad = {0, 1, 2, 3};
    s = sentinel;
    TASK_CHECK(!cache.BuildPalette(ozz::make_span(bad),
                                   ozz::make_span(bad_binds),
                                   ozz::make_span(out), &s));
    TASK_CHECK_UNTOUCHED(s);
    bad_binds = binds;
    bad_binds[0].cols[3] = ozz::math::simd_float4::Load(0.f, 0.f, 0.f, 0.f);
    bad = {0};
    s = sentinel;
    TASK_CHECK(!cache.BuildPalette(ozz::make_span(bad),
                                   ozz::make_span(bad_binds),
                                   ozz::make_span(out), &s));
    TASK_CHECK_UNTOUCHED(s);
  }

  // Output and binds too small.
  {
    bad = {0, 1};
    s = sentinel;
    TASK_CHECK(!cache.BuildPalette(ozz::make_span(bad), ozz::make_span(binds),
                                   ozz::span<Float4x4>(out.data(), 1), &s));
    TASK_CHECK_UNTOUCHED(s);
    s = sentinel;
    TASK_CHECK(!cache.BuildPalette(
        ozz::make_span(bad), ozz::span<const Float4x4>(binds.data(), 1),
        ozz::make_span(out), &s));
    TASK_CHECK_UNTOUCHED(s);
  }
#undef TASK_CHECK_UNTOUCHED

  // The cache survived every failure.
  TASK_CHECK(CheckPalette(cache, all, binds, ref, 2e-4f, &stats) == 0);

  // Retry after failure: valid calls succeed and produce correct values.
  std::vector<Transform> pose2(13);
  for (size_t i = 0; i < pose2.size(); ++i) {
    pose2[i] = RandomTransform(rng, 0.7f, 1.5f, true);
  }
  const std::vector<SoaTransform> soa2 = ToSoa(pose2);
  TASK_CHECK(
      cache.UpdateLocals(ozz::make_span(soa2), ozz::make_span(all), &s));
  std::vector<Float4x4> ref2;
  TASK_CHECK(RefModels(*ts.skeleton, ozz::make_span(soa2), nullptr, &ref2));
  TASK_CHECK(CheckPalette(cache, all, binds, ref2, 2e-4f, &stats) == 0);
  return 0;
}

// Reset unbinds; re-Prepare clears the old character.
int ResetReprepare() {
  PosePaletteCache cache;
  TASK_CHECK(!cache.prepared());

  TestSkeleton a = MakeForkedSkeleton(97);
  TASK_CHECK(cache.Prepare(a.skeleton.get()));
  Rng rng(101);
  std::vector<Transform> expected_a = a.rest;
  TASK_CHECK(ApplyUpdate(cache, a, &expected_a, {0, 4, 8}, rng) == 0);

  cache.Reset();
  TASK_CHECK(!cache.prepared());
  TASK_CHECK(cache.num_joints() == 0);
  const std::vector<int> one = {0};
  std::vector<SoaTransform> soa(1);
  std::vector<Float4x4> one_bind(1, Float4x4::identity());
  std::vector<Float4x4> one_out(1);
  TASK_CHECK(!cache.UpdateLocals(ozz::make_span(soa), ozz::make_span(one),
                                 nullptr));
  TASK_CHECK(!cache.SetRoot(Float4x4::identity(), nullptr));
  TASK_CHECK(!cache.BuildPalette(ozz::make_span(one), ozz::make_span(one_bind),
                                 ozz::make_span(one_out), nullptr));

  // Re-Prepare on a different skeleton: the old character is cleared, the
  // rest pose of the new one is served without any update.
  TestSkeleton b = MakeChainSkeleton(103, 7);
  TASK_CHECK(cache.Prepare(b.skeleton.get()));
  std::vector<Float4x4> ref_b;
  TASK_CHECK(RefFromExpected(b, b.rest, nullptr, &ref_b));
  const std::vector<int> all_b = AllJoints(7);
  const std::vector<Float4x4> binds_b(7, Float4x4::identity());
  PosePaletteCache::Stats stats;
  TASK_CHECK(CheckPalette(cache, all_b, binds_b, ref_b, 2e-4f, &stats) == 0);
  TASK_CHECK(stats.local_matrices == 7 && stats.model_matrices == 7);

  // And back to the first skeleton, rest pose again.
  TASK_CHECK(cache.Prepare(a.skeleton.get()));
  std::vector<Float4x4> ref_a;
  TASK_CHECK(RefFromExpected(a, a.rest, nullptr, &ref_a));
  const std::vector<int> all_a = AllJoints(13);
  const std::vector<Float4x4> binds_a(13, Float4x4::identity());
  TASK_CHECK(CheckPalette(cache, all_a, binds_a, ref_a, 2e-4f, &stats) == 0);
  return 0;
}

// ---------------------------------------------------------------------------
// work group
// ---------------------------------------------------------------------------

// 1024-joint forked skeleton: ancestor closure sizes, cold/warm queries,
// sibling updates, root change, and zero-allocation real stats.
int BigSkeleton1024() {
  // A 128-joint spine with two branches per spine joint, grown
  // breadth-first up to 1024 joints.
  std::vector<int> parents(1024, 0);
  parents[0] = -1;
  for (int i = 1; i < 128; ++i) {
    parents[i] = i - 1;
  }
  int next = 128;
  std::vector<int> frontier;
  for (int i = 0; i < 128 && next < 1024; ++i) {
    for (int k = 0; k < 2 && next < 1024; ++k) {
      parents[next] = i;
      frontier.push_back(next);
      ++next;
    }
  }
  for (size_t f = 0; next < 1024; ++f) {
    parents[next] = frontier[f];
    frontier.push_back(next);
    ++next;
  }
  Rng rng(107);
  std::vector<Transform> rest(1024);
  for (size_t i = 0; i < rest.size(); ++i) {
    rest[i] = RandomTransform(rng, 0.98f, 1.02f, false, 0.3f);
    rest[i].translation =
        Float3(rng.Range(-0.05f, 0.05f), rng.Range(-0.05f, 0.05f),
               rng.Range(-0.05f, 0.05f));
  }
  TestSkeleton ts = MakeSkeleton(parents, rest);
  TASK_CHECK(ts.num_joints() == 1024);

  // Zero-allocation proof: everything ozz allocates from now on is counted.
  CountingAllocator counter(ozz::memory::default_allocator());
  ozz::memory::Allocator* const previous =
      ozz::memory::SetDefaulAllocator(&counter);

  PosePaletteCache cache;
  TASK_CHECK(cache.Prepare(ts.skeleton.get()));
  TASK_CHECK(counter.allocs_ > 0);  // Prepare allocates per skeleton size.
  TASK_CHECK(counter.frees_ == 0);
  counter.allocs_ = 0;
  counter.frees_ = 0;

  // Full pose update, inside the zero-allocation window.
  std::vector<Transform> expected(1024);
  for (size_t i = 0; i < expected.size(); ++i) {
    expected[i] = RandomTransform(rng, 0.98f, 1.02f, false, 0.3f);
    expected[i].translation =
        Float3(rng.Range(-0.05f, 0.05f), rng.Range(-0.05f, 0.05f),
               rng.Range(-0.05f, 0.05f));
  }
  const std::vector<SoaTransform> soa = ToSoa(expected);
  const std::vector<int> all = AllJoints(1024);
  TASK_CHECK(
      cache.UpdateLocals(ozz::make_span(soa), ozz::make_span(all), nullptr));
  std::vector<Float4x4> ref;
  TASK_CHECK(RefModels(*ts.skeleton, ozz::make_span(soa), nullptr, &ref));

  // Query set: the three deepest leaves.
  std::vector<char> has_child(1024, 0);
  for (int j = 0; j < 1024; ++j) {
    if (ts.parents[j] >= 0) {
      has_child[ts.parents[j]] = 1;
    }
  }
  std::vector<int> leaves;
  for (int j = 0; j < 1024; ++j) {
    if (!has_child[j]) {
      leaves.push_back(j);
    }
  }
  TASK_CHECK(leaves.size() >= 3);
  std::sort(leaves.begin(), leaves.end(), [&](int x, int y) {
    return Depth(ts.parents, x) > Depth(ts.parents, y);
  });
  const std::vector<int> joints = {leaves[0], leaves[1], leaves[2]};
  const std::vector<char> closure = AncestorClosure(ts.parents, joints);
  const int closure_size = CountMarked(closure);
  TASK_CHECK(closure_size > 0 && closure_size < 1024);

  std::vector<Float4x4> rest_models;
  TASK_CHECK(RefModels(*ts.skeleton, ts.skeleton->joint_rest_poses(), nullptr,
                       &rest_models));
  std::vector<Float4x4> binds(joints.size());
  for (size_t s = 0; s < joints.size(); ++s) {
    binds[s] = ozz::math::Invert(rest_models[joints[s]]);
  }

  PosePaletteCache::Stats stats;
  std::vector<Float4x4> palette(joints.size());

  // Cold query: the ancestor closure, and only it, is evaluated.
  TASK_CHECK(cache.BuildPalette(ozz::make_span(joints), ozz::make_span(binds),
                                ozz::make_span(palette), &stats));
  TASK_CHECK(stats.local_matrices == closure_size);
  TASK_CHECK(stats.model_matrices == closure_size);
  TASK_CHECK(stats.palette_matrices == static_cast<int>(joints.size()));
  for (size_t s = 0; s < joints.size(); ++s) {
    TASK_CHECK(MatrixNear(palette[s], ref[joints[s]] * binds[s], 1e-4f));
  }

  // Warm query: everything is reused.
  TASK_CHECK(cache.BuildPalette(ozz::make_span(joints), ozz::make_span(binds),
                                ozz::make_span(palette), &stats));
  TASK_CHECK(stats.local_matrices == 0 && stats.model_matrices == 0);
  TASK_CHECK(stats.palette_matrices == static_cast<int>(joints.size()));

  // Sibling update: a joint outside the closure changes nothing.
  int sibling = -1;
  for (int j = 0; j < 1024; ++j) {
    if (!closure[j]) {
      sibling = j;
      break;
    }
  }
  TASK_CHECK(sibling >= 0);
  TASK_CHECK(ApplyUpdate(cache, ts, &expected, {sibling}, rng, 0.98f, 1.02f,
                         false) == 0);
  TASK_CHECK(cache.BuildPalette(ozz::make_span(joints), ozz::make_span(binds),
                                ozz::make_span(palette), &stats));
  TASK_CHECK(stats.local_matrices == 0 && stats.model_matrices == 0);
  for (size_t s = 0; s < joints.size(); ++s) {
    TASK_CHECK(MatrixNear(palette[s], ref[joints[s]] * binds[s], 1e-4f));
  }

  // Ancestor update: only it and the requested descendant chain are
  // re-evaluated, other local matrices are reused.
  const int ancestor = ts.parents[joints[0]];
  TASK_CHECK(ancestor >= 0);
  TASK_CHECK(ApplyUpdate(cache, ts, &expected, {ancestor}, rng, 0.98f, 1.02f,
                         false) == 0);
  TASK_CHECK(RefFromExpected(ts, expected, nullptr, &ref));
  TASK_CHECK(cache.BuildPalette(ozz::make_span(joints), ozz::make_span(binds),
                                ozz::make_span(palette), &stats));
  TASK_CHECK(stats.local_matrices == 1);
  TASK_CHECK(stats.model_matrices ==
             CountClosureBelow(ts.parents, closure, ancestor));
  for (size_t s = 0; s < joints.size(); ++s) {
    TASK_CHECK(MatrixNear(palette[s], ref[joints[s]] * binds[s], 1e-4f));
  }

  // Root change: models re-evaluated, locals reused.
  const Float4x4 root = Float4x4::Translation(Float3(1.f, 2.f, 3.f)) *
                        Float4x4::FromAxisAngle(Float3(0.f, 1.f, 0.f), 0.5f) *
                        Float4x4::Scaling(Float3(1.5f, 0.5f, 2.f));
  TASK_CHECK(cache.SetRoot(root, nullptr));
  TASK_CHECK(RefFromExpected(ts, expected, &root, &ref));
  TASK_CHECK(cache.BuildPalette(ozz::make_span(joints), ozz::make_span(binds),
                                ozz::make_span(palette), &stats));
  TASK_CHECK(stats.local_matrices == 0);
  TASK_CHECK(stats.model_matrices == closure_size);
  for (size_t s = 0; s < joints.size(); ++s) {
    TASK_CHECK(MatrixNear(palette[s], ref[joints[s]] * binds[s], 1e-4f));
  }

  // No heap allocation, no heap release, through every update and query.
  TASK_CHECK(counter.allocs_ == 0);
  TASK_CHECK(counter.frees_ == 0);

  cache.Reset();
  TASK_CHECK(counter.frees_ > 0);  // Reset releases the memory.
  ozz::memory::SetDefaulAllocator(previous);
  return 0;
}

}  // namespace

const struct task_case task_cases[] = {
    {"values", "forked_palette", &ForkedPalette},
    {"values", "partial_lanes", &PartialLanes},
    {"values", "two_skins", &TwoSkins},
    {"values", "skinning_vertices", &SkinningVertices},
    {"values", "input_freed", &InputFreed},
    {"values", "extreme_scales", &ExtremeScales},
    {"values", "interleaved_caches", &InterleavedCaches},
    {"invalidation", "root_change", &RootChange},
    {"invalidation", "ancestor_and_sibling", &AncestorAndSibling},
    {"invalidation", "failed_calls", &FailedCalls},
    {"invalidation", "reset_reprepare", &ResetReprepare},
    {"work", "big_skeleton_1024", &BigSkeleton1024},
};
const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
