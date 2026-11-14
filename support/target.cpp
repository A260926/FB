//----------------------------------------------------------------------------//
// Support tests for ozz::animation::IKMultiEffectorJob.
// Registered through support/check.h; this translation unit defines no main.
//----------------------------------------------------------------------------//

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <new>
#include <vector>

#include "check.h"

#include "ozz/animation/offline/raw_skeleton.h"
#include "ozz/animation/offline/skeleton_builder.h"
#include "ozz/animation/runtime/ik_multi_effector_job.h"
#include "ozz/animation/runtime/local_to_model_job.h"
#include "ozz/animation/runtime/skeleton.h"
#include "ozz/base/maths/quaternion.h"
#include "ozz/base/maths/simd_math.h"
#include "ozz/base/maths/soa_transform.h"
#include "ozz/base/maths/transform.h"
#include "ozz/base/maths/vec_float.h"
#include "ozz/base/memory/unique_ptr.h"

namespace {
using ozz::animation::IKMultiEffectorJob;
using ozz::animation::LocalToModelJob;
using ozz::animation::Skeleton;
using ozz::animation::offline::RawSkeleton;
using ozz::animation::offline::SkeletonBuilder;
namespace math = ozz::math;
using math::Float3;
using math::Quaternion;

const float kPi = ozz::math::kPi;

bool Near(float a, float b, float eps) { return std::abs(a - b) <= eps; }
bool Near(const Float3& a, const Float3& b, float eps) {
  return math::Length(a - b) <= eps;
}
bool AllFinite(const Float3& v) {
  return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

// Global allocation counters, used to prove Run() performs no heap
// allocation once the context is prepared.
unsigned g_new_calls = 0;
unsigned g_delete_calls = 0;
}  // namespace

void* operator new(std::size_t n) {
  ++g_new_calls;
  void* p = std::malloc(n ? n : 1);
  if (!p) throw std::bad_alloc();
  return p;
}
void* operator new[](std::size_t n) {
  ++g_new_calls;
  void* p = std::malloc(n ? n : 1);
  if (!p) throw std::bad_alloc();
  return p;
}
void* operator new(std::size_t n, std::align_val_t a) {
  ++g_new_calls;
  void* p = std::aligned_alloc(static_cast<std::size_t>(a),
                                ((n + static_cast<std::size_t>(a) - 1) /
                                 static_cast<std::size_t>(a)) *
                                    static_cast<std::size_t>(a));
  if (!p) throw std::bad_alloc();
  return p;
}
void* operator new[](std::size_t n, std::align_val_t a) {
  ++g_new_calls;
  void* p = std::aligned_alloc(static_cast<std::size_t>(a),
                                ((n + static_cast<std::size_t>(a) - 1) /
                                 static_cast<std::size_t>(a)) *
                                    static_cast<std::size_t>(a));
  if (!p) throw std::bad_alloc();
  return p;
}
void operator delete(void* p) noexcept {
  ++g_delete_calls;
  std::free(p);
}
void operator delete[](void* p) noexcept {
  ++g_delete_calls;
  std::free(p);
}
void operator delete(void* p, std::size_t) noexcept {
  ++g_delete_calls;
  std::free(p);
}
void operator delete[](void* p, std::size_t) noexcept {
  ++g_delete_calls;
  std::free(p);
}
void operator delete(void* p, std::align_val_t) noexcept {
  ++g_delete_calls;
  std::free(p);
}
void operator delete[](void* p, std::align_val_t) noexcept {
  ++g_delete_calls;
  std::free(p);
}

namespace {
// Independent scalar forward kinematics, deliberately not using the job under
// test.
struct ScalarFk {
  Quaternion rot[Skeleton::kMaxJoints];
  Float3 pos[Skeleton::kMaxJoints];
};

void RunScalarFk(int n, const int16_t* parents, const Quaternion* local_rot,
                 const Float3* local_pos, ScalarFk& out) {
  for (int j = 0; j < n; ++j) {
    const int p = parents[j];
    if (p == Skeleton::kNoParent) {
      out.rot[j] = local_rot[j];
      out.pos[j] = local_pos[j];
    } else {
      out.rot[j] = out.rot[p] * local_rot[j];
      out.pos[j] = out.pos[p] +
                   math::TransformVector(out.rot[p], local_pos[j]);
    }
  }
}

Float3 EffectorPoint(const ScalarFk& fk, int joint, const Float3& offset) {
  return fk.pos[joint] + math::TransformVector(fk.rot[joint], offset);
}

// A scalar mirror of the job optimization problem, used to cross check the
// solver outputs (positions, objective and projected gradient).
struct ScalarProblem {
  int n = 0;
  const int16_t* parents = nullptr;
  std::vector<Quaternion> base_rot;
  std::vector<Float3> local_pos;
  std::vector<IKMultiEffectorJob::Joint> joints;
  std::vector<IKMultiEffectorJob::Effector> effectors;
  float lambda = 1e-3f;

  void PoseFor(const std::vector<float>& angles,
               std::vector<Quaternion>& local_rot) const {
    local_rot = base_rot;
    for (size_t r = 0; r < joints.size(); ++r) {
      const int j = joints[r].joint;
      local_rot[j] = base_rot[j] *
                     Quaternion::FromAxisAngle(joints[r].axis, angles[r]);
    }
  }

  float Objective(const std::vector<float>& angles) const {
    std::vector<Quaternion> local_rot;
    PoseFor(angles, local_rot);
    ScalarFk fk;
    RunScalarFk(n, parents, local_rot.data(), local_pos.data(), fk);
    float obj = 0.f;
    for (const auto& e : effectors) {
      const Float3 residual =
          EffectorPoint(fk, e.joint, e.offset) - e.target;
      obj += e.weight * math::Dot(residual, residual);
    }
    for (float a : angles) obj += lambda * a * a;
    return obj;
  }

  void Gradient(const std::vector<float>& angles,
                std::vector<float>& grad) const {
    const float h = 1e-5f;
    grad.resize(angles.size());
    for (size_t i = 0; i < angles.size(); ++i) {
      std::vector<float> lo = angles, hi = angles;
      lo[i] -= h;
      hi[i] += h;
      grad[i] = (Objective(hi) - Objective(lo)) / (2.f * h);
    }
  }
};

// Double precision mirror of the same problem. The solver runs its internal
// math in double, so its projected gradient is cross checked against a double
// finite difference rather than against the noisier float one.
struct DVec {
  double x = 0.0, y = 0.0, z = 0.0;
};
struct DQuatD {
  double x = 0.0, y = 0.0, z = 0.0, w = 1.0;
};
DVec DMake(double x, double y, double z) { return {x, y, z}; }
DVec DMake(const Float3& v) { return {v.x, v.y, v.z}; }
DVec DAdd(const DVec& a, const DVec& b) {
  return {a.x + b.x, a.y + b.y, a.z + b.z};
}
DVec DSub(const DVec& a, const DVec& b) {
  return {a.x - b.x, a.y - b.y, a.z - b.z};
}
DVec DMul(const DVec& a, double s) {
  return {a.x * s, a.y * s, a.z * s};
}
double DDot(const DVec& a, const DVec& b) {
  return a.x * b.x + a.y * b.y + a.z * b.z;
}
DVec DCross(const DVec& a, const DVec& b) {
  return {a.y * b.z - b.y * a.z, a.z * b.x - b.z * a.x,
          a.x * b.y - b.x * a.y};
}
DVec DRotate(const DQuatD& q, const DVec& v) {
  const DVec qv{q.x, q.y, q.z};
  const DVec t = DAdd(DCross(qv, v), DMul(v, q.w));
  return DAdd(v, DMul(DCross(qv, t), 2.0));
}
DQuatD DMul(const DQuatD& a, const DQuatD& b) {
  return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
          a.w * b.y + a.y * b.w + a.z * b.x - a.x * b.z,
          a.w * b.z + a.z * b.w + a.x * b.y - a.y * b.x,
          a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}
DQuatD DAxisAngle(const DVec& axis, double angle) {
  const double h = angle * 0.5;
  const double s = std::sin(h);
  return {axis.x * s, axis.y * s, axis.z * s, std::cos(h)};
}
struct DScalarFk {
  DQuatD rot[Skeleton::kMaxJoints];
  DVec pos[Skeleton::kMaxJoints];
};
void RunDFk(int n, const int16_t* parents, const DQuatD* local_rot,
            const DVec* local_pos, DScalarFk& out) {
  for (int j = 0; j < n; ++j) {
    const int p = parents[j];
    if (p == Skeleton::kNoParent) {
      out.rot[j] = local_rot[j];
      out.pos[j] = local_pos[j];
    } else {
      out.rot[j] = DMul(out.rot[p], local_rot[j]);
      out.pos[j] = DAdd(out.pos[p], DRotate(out.rot[p], local_pos[j]));
    }
  }
}
DVec DEffectorPoint(const DScalarFk& fk, int joint, const DVec& offset) {
  return DAdd(fk.pos[joint], DRotate(fk.rot[joint], offset));
}

struct DScalarProblem {
  int n = 0;
  const int16_t* parents = nullptr;
  std::vector<DQuatD> base_rot;
  std::vector<DVec> local_pos;
  std::vector<IKMultiEffectorJob::Joint> joints;
  std::vector<IKMultiEffectorJob::Effector> effectors;
  double lambda = 1e-3;

  void PoseFor(const std::vector<double>& angles,
               std::vector<DQuatD>& local_rot) const {
    local_rot = base_rot;
    for (size_t r = 0; r < joints.size(); ++r) {
      const int j = joints[r].joint;
      local_rot[j] = DMul(base_rot[j],
                          DAxisAngle(DMake(joints[r].axis), angles[r]));
    }
  }

  double Objective(const std::vector<double>& angles) const {
    std::vector<DQuatD> local_rot;
    PoseFor(angles, local_rot);
    DScalarFk fk;
    RunDFk(n, parents, local_rot.data(), local_pos.data(), fk);
    double obj = 0.0;
    for (const auto& e : effectors) {
      const DVec point = DEffectorPoint(fk, e.joint, DMake(e.offset));
      const DVec residual = DSub(point, DMake(e.target));
      obj += static_cast<double>(e.weight) * DDot(residual, residual);
    }
    for (double a : angles) obj += lambda * a * a;
    return obj;
  }

  void Gradient(const std::vector<double>& angles,
                std::vector<double>& grad) const {
    const double h = 1e-6;
    grad.resize(angles.size());
    for (size_t i = 0; i < angles.size(); ++i) {
      std::vector<double> lo = angles, hi = angles;
      lo[i] -= h;
      hi[i] += h;
      grad[i] = (Objective(hi) - Objective(lo)) / (2.0 * h);
    }
  }
};

// Builds the forked skeleton used by the coupled group (7 joints, not a
// multiple of 4).
// 0 root, 1 spine, 2 l_shoulder, 3 l_hand, 4 r_shoulder, 5 r_hand, 6 aux.
ozz::unique_ptr<Skeleton> BuildForkSkeleton() {
  RawSkeleton raw;
  raw.roots.resize(1);
  auto& root = raw.roots[0];
  root.children.resize(1);
  auto& spine = root.children[0];
  spine.transform.translation = Float3(0.f, 1.f, 0.f);
  spine.children.resize(3);

  auto& l_shoulder = spine.children[0];
  l_shoulder.transform.translation = Float3(1.f, 0.f, 0.f);
  l_shoulder.children.resize(1);
  l_shoulder.children[0].transform.translation = Float3(1.f, 0.f, 0.f);

  auto& r_shoulder = spine.children[1];
  r_shoulder.transform.translation = Float3(-1.f, 0.f, 0.f);
  r_shoulder.children.resize(1);
  r_shoulder.children[0].transform.translation = Float3(-1.f, 0.f, 0.f);

  spine.children[2].transform.translation = Float3(0.f, 0.f, 1.f);

  SkeletonBuilder builder;
  return builder(raw);
}

// Builds a straight chain of n joints, each bone one model unit long on x.
ozz::unique_ptr<Skeleton> BuildChainSkeleton(int n) {
  RawSkeleton raw;
  raw.roots.resize(1);
  RawSkeleton::Joint* current = &raw.roots[0];
  for (int i = 1; i < n; ++i) {
    current->children.resize(1);
    current = &current->children[0];
    current->transform.translation = Float3(1.f, 0.f, 0.f);
  }
  SkeletonBuilder builder;
  return builder(raw);
}

// Extracts the solved hinge angle from an output local quaternion.
float RecoverAngle(const Quaternion& input_quat, const Quaternion& output_quat,
                   const Float3& axis) {
  const Quaternion delta = math::Conjugate(input_quat) * output_quat;
  const Float3 xyz(delta.x, delta.y, delta.z);
  return 2.f * std::atan2(math::Dot(xyz, axis), delta.w);
}

// Ternary search for the angle minimizing a one-joint scalar objective.
float BestSingleAngle(const ScalarProblem& problem, int rot_index,
                      float lower, float upper) {
  auto f = [&](float angle) {
    std::vector<float> angles(problem.joints.size(), 0.f);
    angles[rot_index] = angle;
    return problem.Objective(angles);
  };
  for (int i = 0; i < 60; ++i) {
    const float m1 = lower + (upper - lower) / 3.f;
    const float m2 = upper - (upper - lower) / 3.f;
    if (f(m1) < f(m2)) {
      upper = m2;
    } else {
      lower = m1;
    }
  }
  return (lower + upper) * .5f;
}

constexpr int kMaxIter = 64;
}  // namespace

// ---------------------------------------------------------------------------
// coupled: shared trunk, two offsets on one joint, and a case where greedy
// per-hand solving destroys the other hand while the coupled solver does not.
// ---------------------------------------------------------------------------
CASE(coupled) {
  auto skeleton = BuildForkSkeleton();
  CHECK(skeleton && skeleton->num_joints() == 7);
  const int n = skeleton->num_joints();
  const auto parents = skeleton->joint_parents();

  IKMultiEffectorJob::Context context;
  CHECK(context.Prepare(skeleton.get(), 4));

  // Rest pose in AoS, for the independent scalar problem.
  std::vector<Quaternion> base_rot(n, Quaternion::identity());
  std::vector<Float3> local_pos(n, Float3::zero());
  local_pos[1] = Float3(0.f, 1.f, 0.f);
  local_pos[2] = Float3(1.f, 0.f, 0.f);
  local_pos[3] = Float3(1.f, 0.f, 0.f);
  local_pos[4] = Float3(-1.f, 0.f, 0.f);
  local_pos[5] = Float3(-1.f, 0.f, 0.f);
  local_pos[6] = Float3(0.f, 0.f, 1.f);

  std::vector<IKMultiEffectorJob::Joint> job_joints = {
      {1, Float3(0.f, 0.f, 1.f), -kPi / 2, kPi / 2},
      {2, Float3(0.f, 0.f, 1.f), -kPi, kPi},
      {4, Float3(0.f, 0.f, 1.f), -kPi, kPi},
  };

  // Generate reachable targets from a known interior pose.
  const std::vector<float> generated = {0.2f, -0.3f, 0.25f};
  std::vector<Quaternion> gen_rot;
  {
    ScalarProblem gen;
    gen.n = n;
    gen.parents = parents.data();
    gen.base_rot = base_rot;
    gen.local_pos = local_pos;
    gen.joints = job_joints;
    gen.lambda = 1e-3f;
    gen.PoseFor(generated, gen_rot);
  }
  ScalarFk gen_fk;
  RunScalarFk(n, parents.data(), gen_rot.data(), local_pos.data(), gen_fk);

  std::vector<IKMultiEffectorJob::Effector> effectors = {
      {3, Float3(0.2f, 0.f, 0.f), EffectorPoint(gen_fk, 3, Float3(0.2f, 0.f, 0.f)), 1.f},
      {5, Float3(-0.2f, 0.f, 0.f),
       EffectorPoint(gen_fk, 5, Float3(-0.2f, 0.f, 0.f)), 1.5f},
      // Second effector on the same joint as the first one, different offset.
      {3, Float3(0.f, 0.3f, 0.f),
       EffectorPoint(gen_fk, 3, Float3(0.f, 0.3f, 0.f)), 0.7f},
  };
  for (const auto& e : effectors) CHECK(AllFinite(e.target));

  std::vector<math::SoaTransform> input(
      skeleton->joint_rest_poses().begin(), skeleton->joint_rest_poses().end());
  std::vector<math::SoaTransform> output(input.size());
  float history[kMaxIter + 1];
  Float3 residuals[4];
  IKMultiEffectorJob::Report report;
  report.iteration_objectives = ozz::make_span(history);
  report.residuals = ozz::make_span(residuals);

  IKMultiEffectorJob job;
  job.skeleton = skeleton.get();
  job.context = &context;
  job.input = ozz::make_span(input);
  job.output = ozz::make_span(output);
  job.joints = ozz::make_span(job_joints);
  job.effectors = ozz::make_span(effectors);
  job.lambda = 1e-3f;
  job.tolerance = 1e-5f;
  job.max_iterations = kMaxIter;
  job.report = &report;
  CHECK(job.Run());

  CHECK(report.termination == IKMultiEffectorJob::Termination::kConverged);
  CHECK(report.iteration_count >= 2);
  CHECK(report.iteration_count <= kMaxIter + 1);

  // Recover the solved angles from the output SoA pose.
  std::vector<float> solved(job_joints.size(), 0.f);
  std::vector<Quaternion> out_quat(n);
  for (int j = 0; j < n; ++j) {
    const auto& block = output[j / 4];
    float q[4];
    math::StorePtrU(block.rotation.x, q);
    const int lane = j & 3;
    float qy[4], qz[4], qw[4];
    math::StorePtrU(block.rotation.y, qy);
    math::StorePtrU(block.rotation.z, qz);
    math::StorePtrU(block.rotation.w, qw);
    out_quat[j] = Quaternion(q[lane], qy[lane], qz[lane], qw[lane]);
  }
  for (size_t r = 0; r < job_joints.size(); ++r) {
    solved[r] = RecoverAngle(base_rot[job_joints[r].joint],
                             out_quat[job_joints[r].joint],
                             job_joints[r].axis);
    CHECK(solved[r] >= job_joints[r].lower - 1e-6f);
    CHECK(solved[r] <= job_joints[r].upper + 1e-6f);
  }

  // Independent scalar FK at the solved angles: positions and objective.
  ScalarProblem problem;
  problem.n = n;
  problem.parents = parents.data();
  problem.base_rot = base_rot;
  problem.local_pos = local_pos;
  problem.joints = job_joints;
  problem.effectors = effectors;
  problem.lambda = job.lambda;

  std::vector<Quaternion> solved_local;
  problem.PoseFor(solved, solved_local);
  ScalarFk solved_fk;
  RunScalarFk(n, parents.data(), solved_local.data(), local_pos.data(),
              solved_fk);

  const float scalar_objective = problem.Objective(solved);
  CHECK(std::isfinite(scalar_objective));
  CHECK(Near(scalar_objective, report.final_objective, 1e-4f));
  CHECK(report.final_objective <= report.initial_objective);

  for (size_t e = 0; e < effectors.size(); ++e) {
    const Float3 point =
        EffectorPoint(solved_fk, effectors[e].joint, effectors[e].offset);
    const Float3 expected_residual = point - effectors[e].target;
    CHECK(Near(point, effectors[e].target, 2e-3f));
    CHECK(Near(report.residuals[e], expected_residual, 1e-5f));
  }

  // The accepted iteration sequence must never increase the full objective.
  for (int i = 1; i < report.iteration_count; ++i) {
    CHECK(report.iteration_objectives[i] <=
          report.iteration_objectives[i - 1] + 1e-9f);
    CHECK(std::isfinite(report.iteration_objectives[i]));
  }
  CHECK(Near(report.iteration_objectives[0], report.initial_objective, 0.f));
  CHECK(Near(report.iteration_objectives[report.iteration_count - 1],
             report.final_objective, 1e-9f));

  // Projected gradient cross checked by independent double precision finite
  // differences of the scalar objective. The solution is interior, so the
  // projection is the identity.
  DScalarProblem dproblem;
  dproblem.n = n;
  dproblem.parents = parents.data();
  dproblem.base_rot.resize(n, DQuatD{});
  dproblem.local_pos.resize(n);
  for (int j = 0; j < n; ++j) dproblem.local_pos[j] = DMake(local_pos[j]);
  dproblem.joints = job_joints;
  dproblem.effectors = effectors;
  dproblem.lambda = static_cast<double>(job.lambda);
  std::vector<double> solved_d(solved.begin(), solved.end());
  const double d_objective = dproblem.Objective(solved_d);
  CHECK(Near(static_cast<float>(d_objective), report.final_objective, 5e-5f));
  std::vector<double> fd_grad;
  dproblem.Gradient(solved_d, fd_grad);
  double fd_norm = 0.0;
  for (double g : fd_grad) fd_norm = std::max(fd_norm, std::abs(g));
  CHECK(Near(static_cast<float>(fd_norm), report.projected_gradient_norm,
             5e-5f));
  CHECK(report.projected_gradient_norm < job.tolerance);

  // --- Greedy per-hand solving vs the coupled system ---------------------
  // Only the shared spine can move. The two hands ask for conflicting
  // rotations. Greedily satisfying one hand always leaves the other one far
  // from its target; the coupled job finds the compromise instead.
  std::vector<IKMultiEffectorJob::Joint> spine_only = {
      {1, Float3(0.f, 0.f, 1.f), -kPi / 2, kPi / 2},
  };
  ScalarFk rest_fk;
  RunScalarFk(n, parents.data(), base_rot.data(), local_pos.data(), rest_fk);
  auto RotatedPoint = [&](int joint, float angle) {
    const Float3 pivot = rest_fk.pos[1];
    const Float3 rel = rest_fk.pos[joint] - pivot;
    const float c = std::cos(angle), s = std::sin(angle);
    return pivot + Float3(c * rel.x - s * rel.y, s * rel.x + c * rel.y, rel.z);
  };
  const float want_left = 0.3f;
  const float want_right = -0.2f;
  std::vector<IKMultiEffectorJob::Effector> conflict = {
      {3, Float3::zero(), RotatedPoint(3, want_left), 1.f},
      {5, Float3::zero(), RotatedPoint(5, want_right), 1.f},
  };

  // Greedy per-hand solving, as with two chained single-effector IK jobs:
  // each hand takes the shared spine over in turn. The last hand wins, so
  // the final spine angle is the optimum of the right hand alone, and the
  // left hand is dragged away from its own target.
  ScalarProblem base_problem;
  base_problem.n = n;
  base_problem.parents = parents.data();
  base_problem.base_rot = base_rot;
  base_problem.local_pos = local_pos;
  base_problem.joints = spine_only;
  base_problem.lambda = 1e-3f;

  std::vector<IKMultiEffectorJob::Effector> left_only = {conflict[0]};
  std::vector<IKMultiEffectorJob::Effector> right_only = {conflict[1]};
  ScalarProblem right_problem = base_problem;
  right_problem.effectors = right_only;
  const float greedy_angle =
      BestSingleAngle(right_problem, 0, -kPi / 2, kPi / 2);
  ScalarProblem both_problem = base_problem;
  both_problem.effectors = conflict;
  const float greedy_sse = both_problem.Objective({greedy_angle});
  // Sanity: the single-hand greedy angle is not the coupled optimum, so
  // both hands really do compete for the same joint.
  ScalarProblem left_problem = base_problem;
  left_problem.effectors = left_only;
  const float left_angle =
      BestSingleAngle(left_problem, 0, -kPi / 2, kPi / 2);
  CHECK(std::abs(left_angle - greedy_angle) > 0.05f);

  std::vector<IKMultiEffectorJob::Effector> conflict_run = conflict;
  float conflict_history[kMaxIter + 1];
  Float3 conflict_residuals[2];
  IKMultiEffectorJob::Report conflict_report;
  conflict_report.iteration_objectives = ozz::make_span(conflict_history);
  conflict_report.residuals = ozz::make_span(conflict_residuals);
  IKMultiEffectorJob conflict_job;
  conflict_job.skeleton = skeleton.get();
  conflict_job.context = &context;
  conflict_job.input = ozz::make_span(input);
  conflict_job.output = ozz::make_span(output);
  conflict_job.joints = ozz::make_span(spine_only);
  conflict_job.effectors = ozz::make_span(conflict_run);
  conflict_job.lambda = 1e-3f;
  conflict_job.tolerance = 1e-4f;
  conflict_job.max_iterations = kMaxIter;
  conflict_job.report = &conflict_report;
  CHECK(conflict_job.Run());
  CHECK(conflict_report.termination ==
        IKMultiEffectorJob::Termination::kConverged);

  // Coupled objective must beat the greedy single-hand solution and each
  // hand must keep a finite, bounded error.
  CHECK(conflict_report.final_objective < greedy_sse - 1e-4f);
  float greedy_max = 0.f, coupled_max = 0.f;
  for (const auto& e : conflict) {
    const Float3 greedy_point = RotatedPoint(e.joint, greedy_angle);
    greedy_max = std::max(greedy_max, math::Length(greedy_point - e.target));
  }
  for (int e = 0; e < 2; ++e) {
    CHECK(AllFinite(conflict_report.residuals[e]));
    coupled_max = std::max(
        coupled_max, math::Length(conflict_report.residuals[e]));
  }
  CHECK(coupled_max < greedy_max);
}

// ---------------------------------------------------------------------------
// limits: boundary optimum, duplicate targets, straight-chain singularity,
// unreachable targets, and an honest iteration-limit report.
// ---------------------------------------------------------------------------
CASE(limits) {
  constexpr int kChain = 5;
  auto skeleton = BuildChainSkeleton(kChain);
  CHECK(skeleton && skeleton->num_joints() == kChain);
  const int n = skeleton->num_joints();
  const auto parents = skeleton->joint_parents();

  std::vector<Quaternion> base_rot(n, Quaternion::identity());
  std::vector<Float3> local_pos(n, Float3::zero());
  for (int j = 1; j < n; ++j) local_pos[j] = Float3(1.f, 0.f, 0.f);

  IKMultiEffectorJob::Context context;
  CHECK(context.Prepare(skeleton.get(), 8));

  std::vector<math::SoaTransform> input(
      skeleton->joint_rest_poses().begin(), skeleton->joint_rest_poses().end());
  std::vector<math::SoaTransform> output(input.size());

  auto run_job = [&](const std::vector<IKMultiEffectorJob::Joint>& jj,
                     const std::vector<IKMultiEffectorJob::Effector>& ee,
                     IKMultiEffectorJob::Report& rep, float lambda = 1e-3f,
                     float tolerance = 1e-5f, int max_iter = kMaxIter) {
    IKMultiEffectorJob job;
    job.skeleton = skeleton.get();
    job.context = &context;
    job.input = ozz::make_span(input);
    job.output = ozz::make_span(output);
    job.joints = ozz::make_span(jj);
    job.effectors = ozz::make_span(ee);
    job.lambda = lambda;
    job.tolerance = tolerance;
    job.max_iterations = max_iter;
    job.report = &rep;
    return job.Run();
  };

  // --- Boundary optimum: a target behind the chain can only be approached
  // by rotating to the negative bound, so the solved angle must sit on it.
  {
    std::vector<IKMultiEffectorJob::Joint> jj = {
        {1, Float3(0.f, 0.f, 1.f), -0.4f, 0.4f}};
    std::vector<IKMultiEffectorJob::Effector> ee = {
        {4, Float3::zero(), Float3(1.f, -5.f, 0.f), 1.f}};
    float history[kMaxIter + 1];
    Float3 residuals[8];
    IKMultiEffectorJob::Report rep;
    rep.iteration_objectives = ozz::make_span(history);
    rep.residuals = ozz::make_span(residuals);
    CHECK(run_job(jj, ee, rep));
    CHECK(rep.termination == IKMultiEffectorJob::Termination::kConverged);

    const auto& block = output[jj[0].joint / 4];
    float qx[4], qy[4], qz[4], qw[4];
    math::StorePtrU(block.rotation.x, qx);
    math::StorePtrU(block.rotation.y, qy);
    math::StorePtrU(block.rotation.z, qz);
    math::StorePtrU(block.rotation.w, qw);
    const int lane = jj[0].joint & 3;
    const Quaternion out_quat(qx[lane], qy[lane], qz[lane], qw[lane]);
    const float angle = RecoverAngle(base_rot[jj[0].joint], out_quat,
                                     jj[0].axis);
    CHECK(Near(angle, jj[0].lower, 2e-3f));
    CHECK(angle >= jj[0].lower - 1e-6f);
    CHECK(rep.projected_gradient_norm < 1e-4f);
  }

  // --- Duplicate targets on the same end joint must not destabilize the
  // solve, and both residuals come out identical.
  {
    std::vector<IKMultiEffectorJob::Joint> jj = {
        {1, Float3(0.f, 0.f, 1.f), -1.f, 1.f},
        {2, Float3(0.f, 0.f, 1.f), -1.f, 1.f}};
    const Float3 target(2.5f, 0.8f, 0.f);
    std::vector<IKMultiEffectorJob::Effector> ee = {
        {4, Float3::zero(), target, 1.f},
        {4, Float3(0.1f, 0.1f, 0.f), target, 2.f}};
    float history[kMaxIter + 1];
    Float3 residuals[8];
    IKMultiEffectorJob::Report rep;
    rep.iteration_objectives = ozz::make_span(history);
    rep.residuals = ozz::make_span(residuals);
    CHECK(run_job(jj, ee, rep));
    CHECK(std::isfinite(rep.final_objective));
    CHECK(AllFinite(rep.residuals[0]) && AllFinite(rep.residuals[1]));
    ScalarFk fk;
    std::vector<Quaternion> out_rot(n);
    for (int j = 0; j < n; ++j) {
      const auto& b = output[j / 4];
      float q[4][4];
      math::StorePtrU(b.rotation.x, q[0]);
      math::StorePtrU(b.rotation.y, q[1]);
      math::StorePtrU(b.rotation.z, q[2]);
      math::StorePtrU(b.rotation.w, q[3]);
      const int lane = j & 3;
      out_rot[j] = Quaternion(q[0][lane], q[1][lane], q[2][lane], q[3][lane]);
    }
    RunScalarFk(n, parents.data(), out_rot.data(), local_pos.data(), fk);
    const Float3 joint_residual = fk.pos[4] - target;
    CHECK(Near(rep.residuals[0], joint_residual, 1e-5f));
    CHECK(Near(rep.residuals[1],
               joint_residual +
                   math::TransformVector(fk.rot[4], ee[1].offset),
               1e-5f));
  }

  // --- Straight-chain singularity: every hinge is parallel (z), the chain
  // is perfectly straight and the target lies exactly on its line. No NaN
  // may be produced, even though the Jacobian is rank deficient there.
  {
    std::vector<IKMultiEffectorJob::Joint> jj;
    for (int j = 1; j < n; ++j) {
      jj.push_back({j, Float3(0.f, 0.f, 1.f), -0.5f, 0.5f});
    }
    std::vector<IKMultiEffectorJob::Effector> ee = {
        {4, Float3::zero(), Float3(4.f, 0.f, 0.f), 1.f}};
    float history[kMaxIter + 1];
    Float3 residuals[8];
    IKMultiEffectorJob::Report rep;
    rep.iteration_objectives = ozz::make_span(history);
    rep.residuals = ozz::make_span(residuals);
    CHECK(run_job(jj, ee, rep));
    CHECK(std::isfinite(rep.final_objective));
    CHECK(AllFinite(rep.residuals[0]));
    for (int i = 0; i < rep.iteration_count; ++i) {
      CHECK(std::isfinite(rep.iteration_objectives[i]));
    }
    // Target at full extension is reachable by the straight pose.
    CHECK(math::Length(rep.residuals[0]) < 1e-3f);
  }

  // --- Unreachable target: farther than the total bone length. The job
  // succeeds, stays finite, reports a non zero residual and never claims
  // convergence from the effector distance alone.
  {
    std::vector<IKMultiEffectorJob::Joint> jj;
    for (int j = 1; j < n; ++j) {
      jj.push_back({j, Float3(0.f, 0.f, 1.f), -1.f, 1.f});
    }
    std::vector<IKMultiEffectorJob::Effector> ee = {
        {4, Float3::zero(), Float3(20.f, 0.f, 0.f), 1.f}};
    float history[kMaxIter + 1];
    Float3 residuals[8];
    IKMultiEffectorJob::Report rep;
    rep.iteration_objectives = ozz::make_span(history);
    rep.residuals = ozz::make_span(residuals);
    CHECK(run_job(jj, ee, rep));
    CHECK(rep.termination == IKMultiEffectorJob::Termination::kConverged);
    CHECK(AllFinite(rep.residuals[0]));
    CHECK(math::Length(rep.residuals[0]) > 10.f);
    CHECK(rep.projected_gradient_norm < 1e-4f);
  }

  // --- Iteration limit: one iteration cannot reach the optimum, so the
  // report must say IterationLimit while still returning the best finite
  // pose and a monotone objective history.
  {
    std::vector<IKMultiEffectorJob::Joint> jj = {
        {1, Float3(0.f, 0.f, 1.f), -1.2f, 1.2f}};
    ScalarFk rest_fk;
    RunScalarFk(n, parents.data(), base_rot.data(), local_pos.data(), rest_fk);
    const Quaternion q = Quaternion::FromAxisAngle(jj[0].axis, 0.9f);
    const Float3 target =
        rest_fk.pos[4] -
        rest_fk.pos[1] +
        math::TransformVector(q, rest_fk.pos[4] - rest_fk.pos[1]) +
        rest_fk.pos[1];
    std::vector<IKMultiEffectorJob::Effector> ee = {
        {4, Float3::zero(), target, 1.f}};
    float history[kMaxIter + 1];
    Float3 residuals[8];
    IKMultiEffectorJob::Report rep;
    rep.iteration_objectives = ozz::make_span(history);
    rep.residuals = ozz::make_span(residuals);
    CHECK(run_job(jj, ee, rep, 1e-3f, 1e-8f, 1));
    CHECK(rep.termination ==
          IKMultiEffectorJob::Termination::kIterationLimit);
    CHECK(rep.iterations == 1);
    CHECK(rep.iteration_count >= 1);
    CHECK(std::isfinite(rep.final_objective));
    CHECK(AllFinite(rep.residuals[0]));
    CHECK(rep.final_objective <= rep.initial_objective + 1e-12f);
  }
}

// ---------------------------------------------------------------------------
// integration: non-multiple-of-four skeleton, aliased vs independent output,
// no-write-on-failure, zero allocation after Prepare, and a real
// LocalToModelJob check of every effector.
// ---------------------------------------------------------------------------
CASE(integration) {
  constexpr int kChain = 7;  // Not a multiple of 4: SoA tail lanes exist.
  auto skeleton = BuildChainSkeleton(kChain);
  CHECK(skeleton && skeleton->num_joints() == kChain);
  const int n = skeleton->num_joints();
  const int num_soa = skeleton->num_soa_joints();
  CHECK(num_soa == 2);

  std::vector<Quaternion> base_rot(n, Quaternion::identity());
  std::vector<Float3> local_pos(n, Float3::zero());
  for (int j = 1; j < n; ++j) local_pos[j] = Float3(1.f, 0.f, 0.f);

  IKMultiEffectorJob::Context context;
  CHECK(context.Prepare(skeleton.get(), 4));

  std::vector<math::SoaTransform> input(
      skeleton->joint_rest_poses().begin(), skeleton->joint_rest_poses().end());

  std::vector<IKMultiEffectorJob::Joint> jj = {
      {1, Float3(0.f, 0.f, 1.f), -1.f, 1.f},
      {2, Float3(0.f, 0.f, 1.f), -1.f, 1.f},
      {3, Float3(1.f, 0.f, 0.f), -0.5f, 0.5f}};

  // Reachable targets generated from a known pose.
  std::vector<float> generated = {0.35f, -0.2f, 0.15f};
  std::vector<Quaternion> gen_rot = base_rot;
  for (size_t r = 0; r < jj.size(); ++r) {
    gen_rot[jj[r].joint] =
        base_rot[jj[r].joint] *
        Quaternion::FromAxisAngle(jj[r].axis, generated[r]);
  }
  ScalarFk gen_fk;
  RunScalarFk(n, skeleton->joint_parents().data(), gen_rot.data(),
              local_pos.data(), gen_fk);
  std::vector<IKMultiEffectorJob::Effector> ee = {
      {6, Float3(0.2f, 0.f, 0.f),
       EffectorPoint(gen_fk, 6, Float3(0.2f, 0.f, 0.f)), 1.f},
      {4, Float3::zero(), gen_fk.pos[4], 1.f}};

  float history_a[kMaxIter + 1], history_b[kMaxIter + 1];
  Float3 residuals_a[4], residuals_b[4];
  IKMultiEffectorJob::Report rep_a, rep_b;
  rep_a.iteration_objectives = ozz::make_span(history_a);
  rep_a.residuals = ozz::make_span(residuals_a);
  rep_b.iteration_objectives = ozz::make_span(history_b);
  rep_b.residuals = ozz::make_span(residuals_b);

  // Independent output buffer.
  std::vector<math::SoaTransform> out_indep(input.size());
  {
    IKMultiEffectorJob job;
    job.skeleton = skeleton.get();
    job.context = &context;
    job.input = ozz::make_span(input);
    job.output = ozz::make_span(out_indep);
    job.joints = ozz::make_span(jj);
    job.effectors = ozz::make_span(ee);
    job.lambda = 1e-3f;
    job.tolerance = 1e-6f;
    job.max_iterations = kMaxIter;
    job.report = &rep_a;
    CHECK(job.Run());
  }

  // In-place output.
  std::vector<math::SoaTransform> inplace = input;
  {
    IKMultiEffectorJob job;
    job.skeleton = skeleton.get();
    job.context = &context;
    job.input = ozz::make_span(inplace);
    job.output = ozz::make_span(inplace);
    job.joints = ozz::make_span(jj);
    job.effectors = ozz::make_span(ee);
    job.lambda = 1e-3f;
    job.tolerance = 1e-6f;
    job.max_iterations = kMaxIter;
    job.report = &rep_b;
    CHECK(job.Run());
  }

  // Both runs must be bit identical for real joints.
  for (int j = 0; j < n; ++j) {
    const int block = j / 4, lane = j & 3;
    float a[4][4], b[4][4];
    math::StorePtrU(out_indep[block].rotation.x, a[0]);
    math::StorePtrU(out_indep[block].rotation.y, a[1]);
    math::StorePtrU(out_indep[block].rotation.z, a[2]);
    math::StorePtrU(out_indep[block].rotation.w, a[3]);
    math::StorePtrU(inplace[block].rotation.x, b[0]);
    math::StorePtrU(inplace[block].rotation.y, b[1]);
    math::StorePtrU(inplace[block].rotation.z, b[2]);
    math::StorePtrU(inplace[block].rotation.w, b[3]);
    for (int c = 0; c < 4; ++c) {
      CHECK(Near(a[c][lane], b[c][lane], 1e-7f));
    }
  }
  CHECK(Near(rep_a.final_objective, rep_b.final_objective, 1e-9f));
  CHECK(rep_a.termination == rep_b.termination);

  // SoA tail lanes (joint 7) keep the input bit pattern, and translations /
  // scales of every real joint are untouched.
  for (int b = 0; b < num_soa; ++b) {
    float in_t[4][4], out_t[4][4];
    math::StorePtrU(input[b].translation.x, in_t[0]);
    math::StorePtrU(input[b].translation.y, in_t[1]);
    math::StorePtrU(input[b].translation.z, in_t[2]);
    math::StorePtrU(input[b].scale.x, in_t[3]);
    math::StorePtrU(inplace[b].translation.x, out_t[0]);
    math::StorePtrU(inplace[b].translation.y, out_t[1]);
    math::StorePtrU(inplace[b].translation.z, out_t[2]);
    math::StorePtrU(inplace[b].scale.x, out_t[3]);
    for (int c = 0; c < 4; ++c) {
      for (int lane = 0; lane < 4; ++lane) {
        CHECK(in_t[c][lane] == out_t[c][lane]);
      }
    }
  }

  // Poison the invalid tail lane of the last block; it must survive a run.
  const int tail_block = num_soa - 1;
  const int tail_lane = n & 3;
  CHECK(tail_lane != 0);
  uint32_t poison_bits = 0x7fc12345;  // A NaN payload.
  float poison;
  std::memcpy(&poison, &poison_bits, sizeof(poison));
  std::vector<math::SoaTransform> poisoned = input;
  float tail_before[4];
  math::StorePtrU(poisoned[tail_block].rotation.x, tail_before);
  tail_before[tail_lane] = poison;
  poisoned[tail_block].rotation.x = math::simd_float4::LoadPtrU(tail_before);
  {
    IKMultiEffectorJob::Report rep;
    float history[kMaxIter + 1];
    Float3 residuals[4];
    rep.iteration_objectives = ozz::make_span(history);
    rep.residuals = ozz::make_span(residuals);
    IKMultiEffectorJob job;
    job.skeleton = skeleton.get();
    job.context = &context;
    job.input = ozz::make_span(poisoned);
    job.output = ozz::make_span(poisoned);
    job.joints = ozz::make_span(jj);
    job.effectors = ozz::make_span(ee);
    job.lambda = 1e-3f;
    job.tolerance = 1e-6f;
    job.max_iterations = kMaxIter;
    job.report = &rep;
    CHECK(job.Run());
    float tail_after[4];
    math::StorePtrU(poisoned[tail_block].rotation.x, tail_after);
    uint32_t after_bits;
    std::memcpy(&after_bits, &tail_after[tail_lane], sizeof(after_bits));
    CHECK(after_bits == poison_bits);
  }

  // Failure paths must leave output and report completely untouched.
  {
    std::vector<math::SoaTransform> untouched = input;
    IKMultiEffectorJob::Report rep;
    float history[4];
    Float3 residuals[4];
    rep.iteration_objectives = ozz::make_span(history);
    rep.residuals = ozz::make_span(residuals);
    rep.termination = IKMultiEffectorJob::Termination::kIterationLimit;
    rep.iterations = 12345;

    IKMultiEffectorJob job;
    job.skeleton = skeleton.get();
    job.context = &context;
    job.input = ozz::make_span(input);
    job.output = ozz::make_span(untouched);
    job.joints = ozz::make_span(jj);
    job.effectors = ozz::make_span(ee);
    job.lambda = 0.f;  // Invalid: lambda must be positive.
    job.tolerance = 1e-6f;
    job.max_iterations = kMaxIter;
    job.report = &rep;
    CHECK(!job.Run());
    CHECK(rep.termination ==
          IKMultiEffectorJob::Termination::kIterationLimit);
    CHECK(rep.iterations == 12345);
    CHECK(std::memcmp(untouched.data(), input.data(),
                      input.size() * sizeof(math::SoaTransform)) == 0);

    // Unprepared context is also a parameter failure.
    IKMultiEffectorJob::Context fresh;
    job.context = &fresh;
    job.lambda = 1e-3f;
    CHECK(!job.Run());
    CHECK(std::memcmp(untouched.data(), input.data(),
                      input.size() * sizeof(math::SoaTransform)) == 0);

    // Report span too small for max_iterations + 1.
    float tiny_history[2];
    rep.iteration_objectives = ozz::make_span(tiny_history);
    job.context = &context;
    job.max_iterations = kMaxIter;
    CHECK(!job.Run());
  }

  // No heap allocation during Run after Prepare.
  {
    std::vector<math::SoaTransform> run_io = input;
    IKMultiEffectorJob::Report rep;
    float history[kMaxIter + 1];
    Float3 residuals[4];
    rep.iteration_objectives = ozz::make_span(history);
    rep.residuals = ozz::make_span(residuals);
    IKMultiEffectorJob job;
    job.skeleton = skeleton.get();
    job.context = &context;
    job.input = ozz::make_span(run_io);
    job.output = ozz::make_span(run_io);
    job.joints = ozz::make_span(jj);
    job.effectors = ozz::make_span(ee);
    job.lambda = 1e-3f;
    job.tolerance = 1e-6f;
    job.max_iterations = kMaxIter;
    job.report = &rep;
    // Snapshot only around Run: the vector copy above is expected to
    // allocate, the solver call itself must not.
    const unsigned before_new = g_new_calls;
    const unsigned before_delete = g_delete_calls;
    CHECK(job.Run());
    CHECK(g_new_calls == before_new);
    CHECK(g_delete_calls == before_delete);
  }

  // End-to-end check through the real LocalToModelJob.
  {
    math::Float4x4 models[Skeleton::kMaxJoints];
    LocalToModelJob fk;
    fk.skeleton = skeleton.get();
    fk.input = ozz::make_span(out_indep);
    fk.output = ozz::make_span(models);
    CHECK(fk.Run());

    for (size_t e = 0; e < ee.size(); ++e) {
      const math::Float4x4& joint_model = models[ee[e].joint];
      const math::SimdFloat4 world =
          math::TransformPoint(joint_model,
                               math::simd_float4::Load(ee[e].offset));
      const Float3 effector_world(math::GetX(world), math::GetY(world),
                                  math::GetZ(world));
      CHECK(Near(effector_world, ee[e].target, 2e-3f));
      // The report residual must match the model-job based measurement.
      CHECK(Near(rep_a.residuals[e], effector_world - ee[e].target, 1e-5f));
    }

    // Fixed joints still participate: every model matrix is finite.
    for (int j = 0; j < n; ++j) {
      for (int c = 0; c < 4; ++c) {
        CHECK(std::isfinite(math::GetX(models[j].cols[c])));
        CHECK(std::isfinite(math::GetY(models[j].cols[c])));
        CHECK(std::isfinite(math::GetZ(models[j].cols[c])));
      }
    }
  }

  // Empty rotatable set is legal: the pose is copied unchanged and the real
  // residual error is reported.
  {
    std::vector<IKMultiEffectorJob::Joint> no_joints;
    std::vector<math::SoaTransform> copied(input.size());
    IKMultiEffectorJob::Report rep;
    float history[kMaxIter + 1];
    Float3 residuals[4];
    rep.iteration_objectives = ozz::make_span(history);
    rep.residuals = ozz::make_span(residuals);
    IKMultiEffectorJob job;
    job.skeleton = skeleton.get();
    job.context = &context;
    job.input = ozz::make_span(input);
    job.output = ozz::make_span(copied);
    job.joints = ozz::make_span(no_joints);
    job.effectors = ozz::make_span(ee);
    job.lambda = 1e-3f;
    job.tolerance = 1e-6f;
    job.max_iterations = kMaxIter;
    job.report = &rep;
    CHECK(job.Run());
    CHECK(rep.termination == IKMultiEffectorJob::Termination::kConverged);
    CHECK(rep.iteration_count == 1);
    CHECK(std::memcmp(copied.data(), input.data(),
                      input.size() * sizeof(math::SoaTransform)) == 0);
    ScalarFk rest_fk;
    RunScalarFk(n, skeleton->joint_parents().data(), base_rot.data(),
                local_pos.data(), rest_fk);
    for (size_t e = 0; e < ee.size(); ++e) {
      const Float3 point =
          EffectorPoint(rest_fk, ee[e].joint, ee[e].offset);
      CHECK(Near(rep.residuals[e], point - ee[e].target, 1e-6f));
      CHECK(math::Length(rep.residuals[e]) > 0.f);
    }
    CHECK(rep.final_objective == rep.initial_objective);
  }

  // Parameter validation: every bad configuration fails and leaves the
  // output untouched.
  {
    auto other = BuildChainSkeleton(5);
    IKMultiEffectorJob::Context small_ctx;
    // Enough capacity for the integration effectors; capacity overruns are
    // exercised in dedicated cases below.
    CHECK(small_ctx.Prepare(skeleton.get(), 4));

    std::vector<math::SoaTransform> sink(input.size());
    IKMultiEffectorJob::Report rep;
    float history[kMaxIter + 1];
    Float3 residuals[4];
    rep.iteration_objectives = ozz::make_span(history);
    rep.residuals = ozz::make_span(residuals);

    auto base_job = [&]() {
      IKMultiEffectorJob job;
      job.skeleton = skeleton.get();
      job.context = &small_ctx;
      job.input = ozz::make_span(input);
      job.output = ozz::make_span(sink);
      job.joints = ozz::make_span(jj);
      job.effectors = ozz::make_span(ee);
      job.lambda = 1e-3f;
      job.tolerance = 1e-6f;
      job.max_iterations = kMaxIter;
      job.report = &rep;
      return job;
    };

    // Skeleton mismatch between the job and the prepared context.
    {
      IKMultiEffectorJob job = base_job();
      job.skeleton = other.get();
      CHECK(!job.Run());
    }
    // Context prepared for fewer effectors than the job provides.
    {
      IKMultiEffectorJob job = base_job();
      IKMultiEffectorJob::Context tight_ctx;
      CHECK(tight_ctx.Prepare(skeleton.get(), 1));
      job.context = &tight_ctx;
      CHECK(static_cast<int>(ee.size()) > 1);
      CHECK(!job.Run());
    }
    // Prepare rejects null skeleton and out of range capacities.
    {
      IKMultiEffectorJob::Context c;
      CHECK(!c.Prepare(nullptr, 4));
      CHECK(!c.Prepare(skeleton.get(), 0));
      CHECK(!c.Prepare(skeleton.get(), 33));
      CHECK(!c.prepared());
    }
    // Duplicate rotatable joint.
    {
      IKMultiEffectorJob job = base_job();
      std::vector<IKMultiEffectorJob::Joint> dup = jj;
      dup.push_back(jj[0]);
      job.joints = ozz::make_span(dup);
      CHECK(!job.Run());
    }
    // Non normalized hinge axis.
    {
      IKMultiEffectorJob job = base_job();
      std::vector<IKMultiEffectorJob::Joint> bad = jj;
      bad[0].axis = Float3(1.f, 1.f, 0.f);
      job.joints = ozz::make_span(bad);
      CHECK(!job.Run());
    }
    // Bounds that do not contain 0.
    {
      IKMultiEffectorJob job = base_job();
      std::vector<IKMultiEffectorJob::Joint> bad = jj;
      bad[0].lower = 0.1f;
      job.joints = ozz::make_span(bad);
      CHECK(!job.Run());
    }
    // Out of range joint index.
    {
      IKMultiEffectorJob job = base_job();
      std::vector<IKMultiEffectorJob::Joint> bad = jj;
      bad[0].joint = n;
      job.joints = ozz::make_span(bad);
      CHECK(!job.Run());
    }
    // Non positive weight and non finite target.
    {
      IKMultiEffectorJob job = base_job();
      std::vector<IKMultiEffectorJob::Effector> bad = ee;
      bad[0].weight = 0.f;
      job.effectors = ozz::make_span(bad);
      CHECK(!job.Run());
      bad[0].weight = 1.f;
      bad[0].target = Float3(std::nanf(""), 0.f, 0.f);
      CHECK(!job.Run());
    }
    // Non positive tolerance and lambda.
    {
      IKMultiEffectorJob job = base_job();
      job.tolerance = 0.f;
      CHECK(!job.Run());
      job.tolerance = 1e-6f;
      job.lambda = -1.f;
      CHECK(!job.Run());
    }
    // Residual span too small for the effectors.
    {
      IKMultiEffectorJob job = base_job();
      IKMultiEffectorJob::Report bad_rep;
      Float3 small_span[1];
      bad_rep.iteration_objectives = ozz::make_span(history);
      bad_rep.residuals = ozz::make_span(small_span);
      job.report = &bad_rep;
      CHECK(static_cast<int>(ee.size()) > 1);
      CHECK(!job.Run());
    }
    // More than 32 effectors exceeds the hard capacity.
    {
      IKMultiEffectorJob job = base_job();
      std::vector<IKMultiEffectorJob::Effector> many(33, ee[0]);
      job.effectors = ozz::make_span(many);
      CHECK(!job.Run());
    }
    // More than 64 rotatable joints exceeds the hard capacity. Uses a
    // dedicated large skeleton so the joint indices stay distinct.
    {
      auto big_skel = BuildChainSkeleton(70);
      IKMultiEffectorJob::Context big_ctx;
      CHECK(big_ctx.Prepare(big_skel.get(), 4));
      std::vector<math::SoaTransform> big_input(
          big_skel->joint_rest_poses().begin(),
          big_skel->joint_rest_poses().end());
      std::vector<math::SoaTransform> big_output(big_input.size());
      std::vector<IKMultiEffectorJob::Joint> many(65);
      for (int i = 0; i < 65; ++i) {
        many[i] = {i, Float3(0.f, 0.f, 1.f), -1.f, 1.f};
      }
      std::vector<IKMultiEffectorJob::Effector> one = {
          {69, Float3::zero(), Float3(1.f, 1.f, 0.f), 1.f}};
      IKMultiEffectorJob::Report big_rep;
      float big_history[kMaxIter + 1];
      Float3 big_residuals[4];
      big_rep.iteration_objectives = ozz::make_span(big_history);
      big_rep.residuals = ozz::make_span(big_residuals);
      IKMultiEffectorJob job;
      job.skeleton = big_skel.get();
      job.context = &big_ctx;
      job.input = ozz::make_span(big_input);
      job.output = ozz::make_span(big_output);
      job.joints = ozz::make_span(many);
      job.effectors = ozz::make_span(one);
      job.lambda = 1e-3f;
      job.tolerance = 1e-6f;
      job.max_iterations = kMaxIter;
      job.report = &big_rep;
      CHECK(!job.Run());
    }
    // Every failed run above used the same sink, which must still hold the
    // bit pattern it had before the calls. Initialize a copy and confirm a
    // further failure leaves it byte identical to the input pose.
    std::vector<math::SoaTransform> probe = input;
    {
      IKMultiEffectorJob job = base_job();
      job.lambda = 0.f;
      job.output = ozz::make_span(probe);
      CHECK(!job.Run());
      CHECK(std::memcmp(probe.data(), input.data(),
                        input.size() * sizeof(math::SoaTransform)) == 0);
    }
  }
}
