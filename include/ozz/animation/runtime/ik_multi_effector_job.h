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

#ifndef OZZ_OZZ_ANIMATION_RUNTIME_IK_MULTI_EFFECTOR_JOB_H_
#define OZZ_OZZ_ANIMATION_RUNTIME_IK_MULTI_EFFECTOR_JOB_H_

#include "ozz/animation/runtime/export.h"
#include "ozz/base/maths/vec_float.h"
#include "ozz/base/platform.h"
#include "ozz/base/span.h"

namespace ozz {
namespace math {
struct SoaTransform;
}
namespace animation {

// Forward declares the Skeleton object used to describe joint hierarchy.
class Skeleton;

// ozz::animation::IKMultiEffectorJob solves several inverse-kinematics end
// points (effectors) at once, over a shared set of rotatable joints.
//
// Unlike chaining IKTwoBoneJob per hand, every effector contributes to a
// single optimization system: effectors that share ancestors (two hands on a
// common spine, or two effectors attached to the same joint through different
// local offsets) are all taken into account by every joint update, so one
// effector can no longer drag the solution of another one away.
//
// Angle convention
// ----------------
// Each rotatable joint owns a single degree of freedom: a normalized hinge
// axis expressed in the joint local-space, and an angle (in radians) bounded
// by [lower, upper], an interval that must contain 0. The output local
// rotation is the input local rotation right-multiplied by the rotation built
// from that axis and the solved angle: output_rotation = input_rotation *
// Quaternion::FromAxisAngle(axis, angle). Local translations and scales are
// never modified. The input pose scales must be unit (1, 1, 1); input
// rotations are arbitrary.
//
// Target space
// ------------
// Each effector references a joint, a fixed offset in that joint local-space
// and a target position in skeleton model-space. The solved point is the
// offset transformed through the full joint hierarchy (model-space). Several
// effectors may share the same joint with different offsets, or share any
// ancestor. All lengths are in model units and every weight must be strictly
// positive.
//
// The solver starts from angle 0 for every joint and minimizes, within the
// angle bounds, the weighted sum of squared target residuals plus a
// Tikhonov regularization term:
//   objective = sum_effector weight * |position(angle) - target|^2
//             + lambda * sum_joint angle^2
// lambda must be strictly positive. The problem is not convex in general, so
// no global optimum is guaranteed.
class OZZ_ANIMATION_DLL IKMultiEffectorJob {
 public:
  // Hard capacity limits. The reusable context preallocates for these sizes,
  // so Run performs no heap allocation.
  enum Limits {
    kMaxJoints = 64,
    kMaxEffectors = 32,
  };

  // Reason why the solver stopped.
  enum class Termination {
    // Projected gradient infinity norm is below the requested tolerance.
    kConverged,

    // The iteration limit was hit. The job still succeeds and outputs the
    // best finite pose found so far.
    kIterationLimit,
  };

  // Description of one rotatable joint.
  struct Joint {
    // Joint index in the skeleton.
    int joint = 0;

    // Normalized hinge axis, in the joint local-space.
    math::Float3 axis = math::Float3::z_axis();

    // Bounds of the angle (radians), relative to the input pose. The
    // interval must contain 0: lower <= 0 <= upper.
    float lower = 0.f;
    float upper = 0.f;
  };

  // Description of one end effector.
  struct Effector {
    // Joint the offset is attached to.
    int joint = 0;

    // Fixed offset in that joint local-space.
    math::Float3 offset = math::Float3::zero();

    // Target position in model-space.
    math::Float3 target = math::Float3::zero();

    // Strictly positive weight of the squared position residual.
    float weight = 1.f;
  };

  // Solver report. All fields are only written on a successful Run; on a
  // parameter failure the report is left untouched.
  struct Report {
    // Objective value at angle 0, before any accepted iteration.
    float initial_objective = 0.f;

    // Objective value of the output pose.
    float final_objective = 0.f;

    // Objective values after each accepted iteration, i.e. each iteration
    // that strictly improved the full objective. The value at index 0 is the
    // objective before the first iteration. Use iteration_count values, the
    // remaining capacity is left untouched.
    span<float> iteration_objectives;

    // Number of values written into iteration_objectives. Always at least 1
    // on success.
    int iteration_count = 0;

    // Model-space residual (position - target) of every effector, in the same
    // order as the job effectors. The span must provide room for all of them.
    span<math::Float3> residuals;

    // Why the solver stopped.
    Termination termination = Termination::kConverged;

    // Number of solver iterations actually performed (accepted or rejected).
    int iterations = 0;

    // Projected gradient infinity norm of the output pose.
    float projected_gradient_norm = 0.f;
  };

  // Reusable preallocated working memory. Prepare() binds it to a skeleton and
  // performs every allocation; Run() then runs without any heap allocation.
  // A default constructed context is not prepared and every Run using it
  // fails.
  struct Context {
    Context() = default;
    ~Context();

    // Contexts own preallocated buffers, they are not copyable.
    Context(const Context&) = delete;
    Context& operator=(const Context&) = delete;

    // Preallocates working buffers for _skeleton and up to _max_effectors.
    // Returns false if _skeleton is null or _max_effectors is out of
    // [1, kMaxEffectors]. On success the context is ready to run jobs for
    // that skeleton only.
    bool Prepare(const Skeleton* _skeleton, int _max_effectors);

    // Returns true once Prepare succeeded.
    bool prepared() const { return skeleton_ != nullptr; }

   private:
    friend IKMultiEffectorJob;
    struct State;
    State* state_ = nullptr;
    const Skeleton* skeleton_ = nullptr;
  };

  // Validates job parameters. Returns true for a valid job, or false
  // otherwise:
  // -if context is null or wasn't prepared, or the skeleton mismatches.
  // -if input/output ranges are too small for the skeleton SoA pose.
  // -if effectors are empty, exceed the context/hard capacity, or any joint
  //  index is not a real joint; if rotatable joints exceed the hard
  //  capacity, reference a joint twice, use a non normalized axis or bounds
  //  that don't contain 0.
  // -if any data is non finite, any weight is not strictly positive, or
  //  lambda/tolerance is not a strictly positive finite value.
  // -if report iteration span capacity is smaller than max_iterations + 1,
  //  or its residual span cannot host every effector.
  // An empty rotatable joint set is legal: the pose is then unchanged and
  // the real residual error is reported. A legal problem that fails to
  // converge is not a parameter error.
  bool Validate() const;

  // Runs the multi-effector solver. The job is validated before any
  // operation is performed, see Validate() for more details. On failure
  // (parameter error, unprepared context, skeleton mismatch) neither the
  // output pose nor the report are modified, and false is returned. Input
  // and output spans may alias the same buffer.
  bool Run() const;

  // Job input.

  // Skeleton describing the joint hierarchy. It must match the skeleton the
  // context was prepared with.
  const Skeleton* skeleton = nullptr;

  // Preallocated context, prepared for skeleton.
  Context* context = nullptr;

  // Input local-space pose, in skeleton SoA layout. Scales must be unit.
  span<const math::SoaTransform> input;

  // Rotatable joints, each with its local hinge axis and angle bounds.
  span<const Joint> joints;

  // End effectors with their local offset, model-space target and weight.
  span<const Effector> effectors;

  // Tikhonov regularization factor, strictly positive.
  float lambda = 1.f;

  // Convergence tolerance on the projected gradient infinity norm, strictly
  // positive. Convergence is never declared from residual distances alone.
  float tolerance = 1e-4f;

  // Maximum number of solver iterations, strictly positive.
  int max_iterations = 32;

  // Job output.

  // Output local-space pose, in skeleton SoA layout. Joints not listed in
  // joints are copied unchanged, including invalid SoA padding lanes. May
  // alias input.
  span<math::SoaTransform> output;

  // Solver report. iteration_objectives must hold at least
  // max_iterations + 1 floats; residuals must hold at least as many entries
  // as effectors.
  Report* report = nullptr;
};
}  // namespace animation
}  // namespace ozz
#endif  // OZZ_OZZ_ANIMATION_RUNTIME_IK_MULTI_EFFECTOR_JOB_H_
