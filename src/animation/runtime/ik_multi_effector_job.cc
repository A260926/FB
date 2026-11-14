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

#include "ozz/animation/runtime/ik_multi_effector_job.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <new>

#include "ozz/animation/runtime/skeleton.h"
#include "ozz/base/maths/quaternion.h"
#include "ozz/base/maths/simd_math.h"
#include "ozz/base/maths/soa_transform.h"
#include "ozz/base/maths/vec_float.h"

namespace ozz {
namespace animation {

namespace {
using math::Float3;
using math::Quaternion;

constexpr int kMaxJoints = IKMultiEffectorJob::kMaxJoints;
constexpr int kMaxEffectors = IKMultiEffectorJob::kMaxEffectors;

// Double precision vectors and quaternions used internally. All solver
// arithmetic (forward kinematics, Jacobian, normal equations, objective)
// runs in double so that the small objective decreases near a solution stay
// representable. Only the job boundary converts to/from float.
struct DVec3 {
  double x = 0.0, y = 0.0, z = 0.0;
};
struct DQuat {
  double x = 0.0, y = 0.0, z = 0.0, w = 1.0;
};

DVec3 V(double _x, double _y, double _z) { return {_x, _y, _z}; }
DVec3 V(const Float3& _v) { return {_v.x, _v.y, _v.z}; }
Float3 F(const DVec3& _v) {
  return Float3(static_cast<float>(_v.x), static_cast<float>(_v.y),
                static_cast<float>(_v.z));
}
DVec3 operator+(const DVec3& _a, const DVec3& _b) {
  return {_a.x + _b.x, _a.y + _b.y, _a.z + _b.z};
}
DVec3 operator-(const DVec3& _a, const DVec3& _b) {
  return {_a.x - _b.x, _a.y - _b.y, _a.z - _b.z};
}
DVec3 operator*(const DVec3& _a, double _s) {
  return {_a.x * _s, _a.y * _s, _a.z * _s};
}
double Dot(const DVec3& _a, const DVec3& _b) {
  return _a.x * _b.x + _a.y * _b.y + _a.z * _b.z;
}
DVec3 Cross(const DVec3& _a, const DVec3& _b) {
  return {_a.y * _b.z - _b.y * _a.z, _a.z * _b.x - _b.z * _a.x,
          _a.x * _b.y - _b.x * _a.y};
}
DVec3 Rotate(const DQuat& _q, const DVec3& _v) {
  // v + 2 * cross(q.xyz, cross(q.xyz, v) + q.w * v)
  const DVec3 qv{_q.x, _q.y, _q.z};
  const DVec3 t = Cross(qv, _v) + _v * _q.w;
  const DVec3 b = Cross(qv, t);
  return _v + b * 2.0;
}
DQuat operator*(const DQuat& _a, const DQuat& _b) {
  return {_a.w * _b.x + _a.x * _b.w + _a.y * _b.z - _a.z * _b.y,
          _a.w * _b.y + _a.y * _b.w + _a.z * _b.x - _a.x * _b.z,
          _a.w * _b.z + _a.z * _b.w + _a.x * _b.y - _a.y * _b.x,
          _a.w * _b.w - _a.x * _b.x - _a.y * _b.y - _a.z * _b.z};
}
DQuat AxisAngle(const DVec3& _axis, double _angle) {
  const double half = _angle * 0.5;
  const double s = std::sin(half);
  return {_axis.x * s, _axis.y * s, _axis.z * s, std::cos(half)};
}
bool IsFinite(double _f) { return std::isfinite(_f); }
bool IsFinite(const DVec3& _v) {
  return std::isfinite(_v.x) && std::isfinite(_v.y) && std::isfinite(_v.z);
}
bool IsFinite(float _f) { return std::isfinite(_f); }
bool IsFinite(const Float3& _v) {
  return std::isfinite(_v.x) && std::isfinite(_v.y) && std::isfinite(_v.z);
}

// All preallocated solver working memory. Its size only depends on the
// skeleton joint count and hard capacities, so it fits in a single heap
// allocation performed by Context::Prepare. Run() therefore allocates
// nothing.
struct SolverState {
  int num_joints = 0;
  int max_effectors = 0;

  // Cached skeleton hierarchy (depth-first parent indices).
  int parents[Skeleton::kMaxJoints];

  // Input local pose in AoS layout. Scales are validated as unit and are
  // therefore not stored.
  DQuat local_rotation[Skeleton::kMaxJoints];
  DVec3 local_translation[Skeleton::kMaxJoints];

  // Rotatable joint data, in job order.
  int rot_joint[kMaxJoints];
  DVec3 rot_axis[kMaxJoints];
  double rot_lower[kMaxJoints];
  double rot_upper[kMaxJoints];
  double angle[kMaxJoints];

  // Maps a skeleton joint index to its rotatable entry, or -1 for fixed
  // joints. Forward kinematics applies the hinge of rotatable joints on top
  // of the input local rotation.
  int rot_index[Skeleton::kMaxJoints];

  // Effector data, in job order.
  int eff_joint[kMaxEffectors];
  DVec3 eff_offset[kMaxEffectors];
  DVec3 eff_target[kMaxEffectors];
  double eff_weight[kMaxEffectors];

  // Current model-space pose and effector positions.
  DQuat model_rotation[Skeleton::kMaxJoints];
  DVec3 model_position[Skeleton::kMaxJoints];
  DVec3 effector_position[kMaxEffectors];

  // Geometric Jacobian, one 3-vector column per rotatable joint, per
  // effector: d(position_e)/d(angle_j). A column is zero when the rotatable
  // joint is not an ancestor of the effector's joint.
  DVec3 jacobian[kMaxEffectors][kMaxJoints];

  // Gauss-Newton / Levenberg-Marquardt work matrices and vectors.
  double hessian[kMaxJoints][kMaxJoints];
  double gradient[kMaxJoints];
  double rhs[kMaxJoints];
  double step[kMaxJoints];
  double candidate[kMaxJoints];
};

// Forward kinematics over the whole hierarchy, including fixed joints.
// Model-space transforms of unlisted joints are rebuilt from the input pose,
// so they participate normally in hierarchy propagation. Rotatable joints
// get their current hinge rotation right-multiplied to their input rotation.
void ForwardKinematics(SolverState& _s) {
  for (int j = 0; j < _s.num_joints; ++j) {
    const int parent = _s.parents[j];
    const int r = _s.rot_index[j];
    const DQuat local =
        r < 0 ? _s.local_rotation[j]
              : _s.local_rotation[j] *
                    AxisAngle(_s.rot_axis[r], _s.angle[r]);
    if (parent == Skeleton::kNoParent) {
      _s.model_rotation[j] = local;
      _s.model_position[j] = _s.local_translation[j];
    } else {
      _s.model_rotation[j] = _s.model_rotation[parent] * local;
      _s.model_position[j] =
          _s.model_position[parent] +
          Rotate(_s.model_rotation[parent], _s.local_translation[j]);
    }
  }
}

// Evaluates the full objective for the current angles, refreshes effector
// positions and, when requested, computes the exact geometric Jacobian.
// For a revolute joint the effector point velocity per radian is the cross
// product of the joint hinge axis in model space with the vector from the
// joint to the effector point; non ancestor joints contribute zero.
double Evaluate(SolverState& _s, int _num_rot, int _num_eff, double _lambda,
                bool _compute_jacobian) {
  ForwardKinematics(_s);

  for (int e = 0; e < _num_eff; ++e) {
    const int joint = _s.eff_joint[e];
    _s.effector_position[e] =
        _s.model_position[joint] +
        Rotate(_s.model_rotation[joint], _s.eff_offset[e]);
  }

  double objective = 0.0;
  for (int e = 0; e < _num_eff; ++e) {
    const DVec3 residual = _s.effector_position[e] - _s.eff_target[e];
    objective += _s.eff_weight[e] * Dot(residual, residual);
  }
  for (int r = 0; r < _num_rot; ++r) {
    objective += _lambda * _s.angle[r] * _s.angle[r];
  }
  if (!IsFinite(objective)) {
    return std::numeric_limits<double>::infinity();
  }

  if (!_compute_jacobian) {
    return objective;
  }

  for (int e = 0; e < _num_eff; ++e) {
    // Walk the effector's ancestry and flag the rotatable joints on it.
    bool affects[kMaxJoints];
    for (int r = 0; r < _num_rot; ++r) affects[r] = false;
    int joint = _s.eff_joint[e];
    while (joint != Skeleton::kNoParent) {
      const int r = _s.rot_index[joint];
      if (r >= 0) {
        affects[r] = true;
      }
      joint = _s.parents[joint];
    }
    for (int r = 0; r < _num_rot; ++r) {
      if (!affects[r]) {
        _s.jacobian[e][r] = DVec3{};
        continue;
      }
      const int hinge_joint = _s.rot_joint[r];
      const DVec3 model_axis =
          Rotate(_s.model_rotation[hinge_joint], _s.rot_axis[r]);
      const DVec3 lever =
          _s.effector_position[e] - _s.model_position[hinge_joint];
      DVec3 column = Cross(model_axis, lever);
      if (!IsFinite(column)) {
        column = DVec3{};
      }
      _s.jacobian[e][r] = column;
    }
  }
  return objective;
}

// Returns the projected descent component for angle _r. The stored gradient
// is -1/2 of the objective gradient, so it already points downhill. At a
// lower bound only positive components (a move back inside the interval)
// are feasible; at an upper bound only negative components are.
double ProjectedComponent(const SolverState& _s, int _r, double _g) {
  const double kBoundEps = 1e-10;
  if (_s.angle[_r] <= _s.rot_lower[_r] + kBoundEps) {
    return std::max(0.0, _g);
  }
  if (_s.angle[_r] >= _s.rot_upper[_r] - kBoundEps) {
    return std::min(0.0, _g);
  }
  return _g;
}

// Cholesky solve of a symmetric positive definite system. Returns false if
// the factorization breaks down, in which case the caller increases
// regularization instead of using the result.
bool SolveSystem(SolverState& _s, int _n) {
  double lower[kMaxJoints][kMaxJoints];
  std::memset(lower, 0, sizeof(lower));
  for (int i = 0; i < _n; ++i) {
    for (int j = 0; j <= i; ++j) {
      double sum = _s.hessian[i][j];
      for (int k = 0; k < j; ++k) {
        sum -= lower[i][k] * lower[j][k];
      }
      if (i == j) {
        if (sum <= 1e-14) {
          return false;
        }
        lower[i][j] = std::sqrt(sum);
      } else {
        lower[i][j] = sum / lower[j][j];
      }
    }
  }
  for (int i = 0; i < _n; ++i) {
    double sum = _s.rhs[i];
    for (int k = 0; k < i; ++k) {
      sum -= lower[i][k] * _s.step[k];
    }
    _s.step[i] = sum / lower[i][i];
  }
  for (int i = _n - 1; i >= 0; --i) {
    double sum = _s.step[i];
    for (int k = i + 1; k < _n; ++k) {
      sum -= lower[k][i] * _s.step[k];
    }
    _s.step[i] = sum / lower[i][i];
  }
  return true;
}

// Builds the damped Gauss-Newton normal equations for the current pose. The
// stored gradient is -1/2 of the objective gradient; solving with it gives
// the same descent direction as a classical Newton step.
void BuildNormalEquations(SolverState& _s, int _num_rot, int _num_eff,
                          double _lambda, double _damping) {
  for (int i = 0; i < _num_rot; ++i) {
    _s.gradient[i] = 0.0;
    for (int j = 0; j < _num_rot; ++j) {
      _s.hessian[i][j] = 0.0;
    }
  }

  for (int e = 0; e < _num_eff; ++e) {
    const DVec3 residual = _s.eff_target[e] - _s.effector_position[e];
    const double w = _s.eff_weight[e];
    for (int i = 0; i < _num_rot; ++i) {
      const DVec3 ji = _s.jacobian[e][i] * w;
      _s.gradient[i] += Dot(ji, residual);
      for (int j = 0; j <= i; ++j) {
        _s.hessian[i][j] += Dot(ji, _s.jacobian[e][j]);
      }
    }
  }

  for (int i = 0; i < _num_rot; ++i) {
    _s.gradient[i] -= _lambda * _s.angle[i];
    _s.hessian[i][i] += _lambda + _damping;
    for (int j = i + 1; j < _num_rot; ++j) {
      _s.hessian[i][j] = _s.hessian[j][i];
    }
    _s.rhs[i] = _s.gradient[i];
  }
}

}  // namespace

struct IKMultiEffectorJob::Context::State {
  SolverState solver;
};

IKMultiEffectorJob::Context::~Context() {
  delete state_;
  state_ = nullptr;
  skeleton_ = nullptr;
}

bool IKMultiEffectorJob::Context::Prepare(const Skeleton* _skeleton,
                                          int _max_effectors) {
  if (!_skeleton || _max_effectors < 1 || _max_effectors > kMaxEffectors ||
      _skeleton->num_joints() <= 0 ||
      _skeleton->num_joints() > Skeleton::kMaxJoints) {
    return false;
  }

  State* new_state = new (std::nothrow) State();
  if (!new_state) {
    return false;
  }

  new_state->solver.num_joints = _skeleton->num_joints();
  new_state->solver.max_effectors = _max_effectors;
  const span<const int16_t>& skel_parents = _skeleton->joint_parents();
  for (int i = 0; i < _skeleton->num_joints(); ++i) {
    new_state->solver.parents[i] = skel_parents[i];
  }

  // Releasing the previous state only after the new allocation succeeded
  // keeps an already prepared context usable if allocation ever fails.
  delete state_;
  state_ = new_state;
  skeleton_ = _skeleton;
  return true;
}

bool IKMultiEffectorJob::Validate() const {
  if (!skeleton || !context || !context->prepared()) {
    return false;
  }
  if (context->skeleton_ != skeleton) {
    return false;
  }

  const int num_joints = skeleton->num_joints();
  const int num_soa = skeleton->num_soa_joints();
  if (static_cast<int>(input.size()) < num_soa ||
      static_cast<int>(output.size()) < num_soa) {
    return false;
  }
  if (!report ||
      report->iteration_objectives.size() <
          static_cast<size_t>(max_iterations) + 1 ||
      report->residuals.size() < effectors.size()) {
    return false;
  }
  if (!IsFinite(lambda) || lambda <= 0.f || !IsFinite(tolerance) ||
      tolerance <= 0.f || max_iterations <= 0) {
    return false;
  }

  const int num_rot = static_cast<int>(joints.size());
  const int num_eff = static_cast<int>(effectors.size());
  if (num_rot > kMaxJoints || num_eff <= 0 || num_eff > kMaxEffectors ||
      num_eff > context->state_->solver.max_effectors) {
    return false;
  }

  // Rotatable joints: real indices, no duplicate, finite and normalized local
  // hinge axes, and bounds containing 0.
  for (int i = 0; i < num_rot; ++i) {
    const Joint& j = joints[i];
    if (j.joint < 0 || j.joint >= num_joints || !IsFinite(j.axis) ||
        !IsFinite(j.lower) || !IsFinite(j.upper) || j.lower > 0.f ||
        j.upper < 0.f || j.lower > j.upper) {
      return false;
    }
    if (std::abs(math::LengthSqr(j.axis) - 1.f) > 1e-4f) {
      return false;
    }
    for (int k = 0; k < i; ++k) {
      if (joints[k].joint == j.joint) {
        return false;
      }
    }
  }

  // Effectors: real joints, finite data, strictly positive weights.
  for (int i = 0; i < num_eff; ++i) {
    const Effector& e = effectors[i];
    if (e.joint < 0 || e.joint >= num_joints || !IsFinite(e.offset) ||
        !IsFinite(e.target) || !IsFinite(e.weight) || e.weight <= 0.f) {
      return false;
    }
  }

  // Input scales must be unit; input rotations and translations must be
  // finite. Padding lanes are not real joints and are never inspected.
  for (int i = 0; i < num_soa; ++i) {
    const math::SoaTransform& t = input[i];
    float lanes[10][4];
    math::StorePtrU(t.scale.x, lanes[0]);
    math::StorePtrU(t.scale.y, lanes[1]);
    math::StorePtrU(t.scale.z, lanes[2]);
    math::StorePtrU(t.translation.x, lanes[3]);
    math::StorePtrU(t.translation.y, lanes[4]);
    math::StorePtrU(t.translation.z, lanes[5]);
    math::StorePtrU(t.rotation.x, lanes[6]);
    math::StorePtrU(t.rotation.y, lanes[7]);
    math::StorePtrU(t.rotation.z, lanes[8]);
    math::StorePtrU(t.rotation.w, lanes[9]);
    for (int lane = 0; lane < 4 && i * 4 + lane < num_joints; ++lane) {
      for (int comp = 0; comp < 3; ++comp) {
        if (std::abs(lanes[comp][lane] - 1.f) > 1e-5f) {
          return false;
        }
      }
      for (int comp = 3; comp < 10; ++comp) {
        if (!std::isfinite(lanes[comp][lane])) {
          return false;
        }
      }
    }
  }
  return true;
}

bool IKMultiEffectorJob::Run() const {
  if (!Validate()) {
    return false;
  }

  SolverState& s = context->state_->solver;
  const int num_joints = skeleton->num_joints();
  const int num_soa = skeleton->num_soa_joints();
  const int num_rot = static_cast<int>(joints.size());
  const int num_eff = static_cast<int>(effectors.size());

  // Cache job data in the preallocated context buffers.
  for (int j = 0; j < num_joints; ++j) {
    s.rot_index[j] = -1;
  }
  for (int i = 0; i < num_rot; ++i) {
    s.rot_joint[i] = joints[i].joint;
    s.rot_axis[i] = V(joints[i].axis);
    s.rot_lower[i] = joints[i].lower;
    s.rot_upper[i] = joints[i].upper;
    s.angle[i] = 0.0;
    s.rot_index[joints[i].joint] = i;
  }
  for (int i = 0; i < num_eff; ++i) {
    s.eff_joint[i] = effectors[i].joint;
    s.eff_offset[i] = V(effectors[i].offset);
    s.eff_target[i] = V(effectors[i].target);
    s.eff_weight[i] = effectors[i].weight;
  }

  // Unpack the input SoA pose into AoS doubles. Padding lanes are not real
  // joints, they are never read here and their bit patterns survive through
  // the output copy done at the end.
  for (int i = 0; i < num_soa; ++i) {
    const math::SoaTransform& t = input[i];
    float tx[4], ty[4], tz[4], qx[4], qy[4], qz[4], qw[4];
    math::StorePtrU(t.translation.x, tx);
    math::StorePtrU(t.translation.y, ty);
    math::StorePtrU(t.translation.z, tz);
    math::StorePtrU(t.rotation.x, qx);
    math::StorePtrU(t.rotation.y, qy);
    math::StorePtrU(t.rotation.z, qz);
    math::StorePtrU(t.rotation.w, qw);
    for (int lane = 0; lane < 4 && i * 4 + lane < num_joints; ++lane) {
      const int j = i * 4 + lane;
      s.local_translation[j] = V(tx[lane], ty[lane], tz[lane]);
      s.local_rotation[j] =
          DQuat{qx[lane], qy[lane], qz[lane], qw[lane]};
    }
  }

  // The report objective span is caller-owned storage with room for
  // max_iterations + 1 values; Run cannot fail past validation, so writing
  // the iteration history directly into it is safe and needs no allocation.
  float* const history = report->iteration_objectives.data();
  int history_count = 0;

  // Initial objective at angle 0.
  double current_objective = Evaluate(s, num_rot, num_eff, lambda, true);
  double best_objective = current_objective;
  history[history_count++] = static_cast<float>(current_objective);

  double best_angle[kMaxJoints];
  for (int i = 0; i < num_rot; ++i) {
    best_angle[i] = 0.0;
  }

  double damping = 1.0;
  int iterations = 0;
  Termination termination = Termination::kIterationLimit;

  for (iterations = 0; iterations < max_iterations; ++iterations) {
    // The projected gradient of the current pose drives both convergence
    // detection and the search direction.
    BuildNormalEquations(s, num_rot, num_eff, lambda, 0.0);
    double projected_norm = 0.0;
    for (int i = 0; i < num_rot; ++i) {
      projected_norm = std::max(
          projected_norm, std::abs(ProjectedComponent(s, i, s.gradient[i])));
    }
    if (projected_norm < static_cast<double>(tolerance)) {
      termination = Termination::kConverged;
      break;
    }

    // Damped Gauss-Newton step with backtracking. Every accepted step
    // strictly decreases the full objective; rejected trials restore the
    // best pose.
    bool accepted = false;
    bool stationary = false;
    double attempt_damping = damping;
    for (int trial = 0; trial < 24 && !accepted; ++trial) {
      BuildNormalEquations(s, num_rot, num_eff, lambda, attempt_damping);
      if (!SolveSystem(s, num_rot)) {
        attempt_damping *= 4.0;
        continue;
      }

      double move = 0.0;
      for (int i = 0; i < num_rot; ++i) {
        s.candidate[i] = std::min(
            s.rot_upper[i],
            std::max(s.rot_lower[i], s.angle[i] + s.step[i]));
        if (!IsFinite(s.candidate[i])) {
          s.candidate[i] = s.angle[i];
        }
        move = std::max(move, std::abs(s.candidate[i] - s.angle[i]));
      }
      if (move < 1e-12) {
        stationary = true;
        break;
      }

      for (int i = 0; i < num_rot; ++i) {
        s.angle[i] = s.candidate[i];
      }
      const double candidate_objective =
          Evaluate(s, num_rot, num_eff, lambda, false);
      // A strictly positive, scale relative improvement threshold keeps the
      // monotone guarantee while accepting numerically meaningful steps.
      const double improvement =
          1e-14 * std::max(1.0, std::abs(current_objective));
      if (IsFinite(candidate_objective) &&
          candidate_objective < current_objective - improvement) {
        current_objective = candidate_objective;
        damping = std::max(1e-12, attempt_damping * 0.5);
        accepted = true;
      } else {
        attempt_damping *= 4.0;
        for (int i = 0; i < num_rot; ++i) {
          s.angle[i] = best_angle[i];
        }
      }
    }

    if (stationary) {
      // No feasible movement is left: the constrained optimum is reached.
      termination = Termination::kConverged;
      break;
    }
    if (!accepted) {
      // Gauss-Newton could not improve the objective, typically because the
      // step keeps hitting an angle bound. Fall back to a projected
      // gradient line search so an active-set optimum can still be reached.
      double direction[kMaxJoints];
      double direction_norm = 0.0;
      for (int i = 0; i < num_rot; ++i) {
        direction[i] = ProjectedComponent(s, i, s.gradient[i]);
        direction_norm =
            std::max(direction_norm, std::abs(direction[i]));
      }
      if (direction_norm < static_cast<double>(tolerance)) {
        termination = Termination::kConverged;
        break;
      }

      double alpha = 1.0;
      for (int trial = 0; trial < 40; ++trial) {
        double move = 0.0;
        for (int i = 0; i < num_rot; ++i) {
          s.candidate[i] = std::min(
              s.rot_upper[i],
              std::max(s.rot_lower[i], s.angle[i] + alpha * direction[i]));
          move = std::max(move, std::abs(s.candidate[i] - s.angle[i]));
        }
        if (move < 1e-12) {
          break;
        }
        for (int i = 0; i < num_rot; ++i) {
          s.angle[i] = s.candidate[i];
        }
        const double candidate_objective =
            Evaluate(s, num_rot, num_eff, lambda, false);
        const double improvement =
            1e-14 * std::max(1.0, std::abs(current_objective));
        if (IsFinite(candidate_objective) &&
            candidate_objective < current_objective - improvement) {
          current_objective = candidate_objective;
          accepted = true;
          break;
        }
        for (int i = 0; i < num_rot; ++i) {
          s.angle[i] = best_angle[i];
        }
        alpha *= 0.5;
      }
      if (!accepted) {
        // No feasible descent step exists: the constrained optimum is
        // reached (or below numerical resolution).
        termination = Termination::kConverged;
        break;
      }
    }

    for (int i = 0; i < num_rot; ++i) {
      best_angle[i] = s.angle[i];
    }
    best_objective = current_objective;
    history[history_count++] = static_cast<float>(current_objective);
    Evaluate(s, num_rot, num_eff, lambda, true);  // Refresh the Jacobian.
  }

  // Restore the best finite pose and evaluate its final state. The restore
  // is also what gets returned on IterationLimit.
  for (int i = 0; i < num_rot; ++i) {
    s.angle[i] = best_angle[i];
  }
  const double final_objective =
      Evaluate(s, num_rot, num_eff, lambda, true);
  current_objective =
      IsFinite(final_objective) ? final_objective : best_objective;

  // Re-check the projected gradient before declaring convergence: residual
  // distances alone never trigger Converged.
  BuildNormalEquations(s, num_rot, num_eff, lambda, 0.0);
  double final_projected_norm = 0.0;
  for (int i = 0; i < num_rot; ++i) {
    final_projected_norm = std::max(
        final_projected_norm,
        std::abs(ProjectedComponent(s, i, s.gradient[i])));
  }
  if (termination == Termination::kConverged &&
      !(final_projected_norm < static_cast<double>(tolerance))) {
    termination = Termination::kIterationLimit;
  }

  // Copy the input pose to the output first: fixed joints, translations,
  // scales and invalid SoA padding lanes are all preserved bit for bit.
  if (output.data() != input.data()) {
    std::memcpy(output.data(), input.data(),
                static_cast<size_t>(num_soa) * sizeof(math::SoaTransform));
  }

  // Apply the solved hinge rotations to their owning SoA lanes. The product
  // matches the documented convention output = input * axis-angle exactly;
  // the hinge is unit and valid input rotations are unit, so no renormalization
  // alters the requested rotation.
  for (int r = 0; r < num_rot; ++r) {
    const int joint = s.rot_joint[r];
    const int block = joint / 4;
    const int lane = joint & 3;
    const DQuat hinge = AxisAngle(s.rot_axis[r], s.angle[r]);
    const DQuat result = s.local_rotation[joint] * hinge;

    math::SoaTransform& out = output[block];
    out.rotation.x = math::SetI(
        out.rotation.x, math::simd_float4::LoadX(static_cast<float>(result.x)),
        lane);
    out.rotation.y = math::SetI(
        out.rotation.y, math::simd_float4::LoadX(static_cast<float>(result.y)),
        lane);
    out.rotation.z = math::SetI(
        out.rotation.z, math::simd_float4::LoadX(static_cast<float>(result.z)),
        lane);
    out.rotation.w = math::SetI(
        out.rotation.w, math::simd_float4::LoadX(static_cast<float>(result.w)),
        lane);
  }

  // Fill the report.
  Report& rep = *report;
  rep.initial_objective = history[0];
  rep.final_objective = static_cast<float>(current_objective);
  rep.iteration_count = history_count;
  for (int e = 0; e < num_eff; ++e) {
    rep.residuals[e] = F(s.effector_position[e] - s.eff_target[e]);
  }
  rep.termination = termination;
  rep.iterations = iterations;
  rep.projected_gradient_norm = static_cast<float>(final_projected_norm);
  return true;
}
}  // namespace animation
}  // namespace ozz
