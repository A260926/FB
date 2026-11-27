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

#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "ozz/animation/offline/animation_builder.h"
#include "ozz/animation/offline/raw_animation.h"
#include "ozz/animation/offline/raw_animation_retargeter.h"
#include "ozz/animation/offline/raw_skeleton.h"
#include "ozz/animation/offline/skeleton_builder.h"
#include "ozz/animation/runtime/animation.h"
#include "ozz/animation/runtime/local_to_model_job.h"
#include "ozz/animation/runtime/sampling_job.h"
#include "ozz/animation/runtime/skeleton.h"
#include "ozz/base/containers/vector.h"
#include "ozz/base/maths/soa_transform.h"
#include "ozz/base/maths/transform.h"

// Code for ozz-animation HowTo: "How to retarget an animation from a skeleton
// to another?"

namespace {

using ozz::animation::offline::RawAnimation;
using ozz::animation::offline::RawAnimationRetargeter;
using ozz::animation::offline::RawSkeleton;
using ozz::math::Float3;
using ozz::math::Quaternion;

// Source skeleton: 3 joints, all in a straight line.
ozz::unique_ptr<ozz::animation::Skeleton> BuildSourceSkeleton() {
  RawSkeleton raw;
  raw.roots.resize(1);
  RawSkeleton::Joint& root = raw.roots[0];
  root.name = "source_root";
  root.transform.translation = Float3(0.f, 1.f, 0.f);
  root.transform.rotation =
      Quaternion::FromAxisAngle(Float3(0.f, 1.f, 0.f), .2f);

  root.children.resize(1);
  RawSkeleton::Joint& spine = root.children[0];
  spine.name = "source_spine";
  spine.transform.translation = Float3(0.f, .5f, 0.f);
  spine.transform.rotation =
      Quaternion::FromAxisAngle(Float3(1.f, 0.f, 0.f), -.3f);

  spine.children.resize(1);
  RawSkeleton::Joint& head = spine.children[0];
  head.name = "source_head";
  head.transform.translation = Float3(0.f, .3f, 0.f);

  return ozz::animation::offline::SkeletonBuilder()(raw);
}

// Target skeleton: 4 joints. The rest pose differs from the source one, and an
// extra intermediate joint is inserted between the spine and the head.
ozz::unique_ptr<ozz::animation::Skeleton> BuildTargetSkeleton() {
  RawSkeleton raw;
  raw.roots.resize(1);
  RawSkeleton::Joint& root = raw.roots[0];
  root.name = "target_root";
  root.transform.translation = Float3(0.f, 1.2f, 0.f);
  root.transform.rotation =
      Quaternion::FromAxisAngle(Float3(0.f, 1.f, 0.f), .1f);

  root.children.resize(1);
  RawSkeleton::Joint& spine = root.children[0];
  spine.name = "target_spine";
  spine.transform.translation = Float3(0.f, .6f, 0.f);
  spine.transform.rotation =
      Quaternion::FromAxisAngle(Float3(1.f, 0.f, 0.f), .25f);

  spine.children.resize(1);
  RawSkeleton::Joint& neck = spine.children[0];
  neck.name = "target_neck";
  neck.transform.translation = Float3(0.f, .2f, 0.f);
  neck.transform.rotation =
      Quaternion::FromAxisAngle(Float3(0.f, 0.f, 1.f), .4f);

  neck.children.resize(1);
  RawSkeleton::Joint& head = neck.children[0];
  head.name = "target_head";
  head.transform.translation = Float3(0.f, .25f, 0.f);

  return ozz::animation::offline::SkeletonBuilder()(raw);
}

// Source animation: a nodding motion and a moving root.
RawAnimation BuildSourceAnimation() {
  RawAnimation animation;
  animation.duration = 1.f;
  animation.tracks.resize(3);

  animation.tracks[0].translations.push_back({0.f, Float3(0.f, 1.f, 0.f)});
  animation.tracks[0].translations.push_back({1.f, Float3(.5f, 1.f, 0.f)});

  const float times[] = {0.f, .25f, .5f, .75f, 1.f};
  for (const float time : times) {
    const float angle = .4f * std::sin(6.2831853f * time);
    animation.tracks[0].rotations.push_back(
        {time, Quaternion::FromAxisAngle(Float3(0.f, 1.f, 0.f), .2f + angle)});
    animation.tracks[1].rotations.push_back(
        {time, Quaternion::FromAxisAngle(Float3(1.f, 0.f, 0.f), -.3f + angle)});
    animation.tracks[2].rotations.push_back(
        {time, Quaternion::FromAxisAngle(Float3(0.f, 0.f, 1.f), angle)});
  }
  return animation;
}
}  // namespace

int main(int argc, char const* argv[]) {
  (void)argc;
  (void)argv;

  const ozz::unique_ptr<ozz::animation::Skeleton> source =
      BuildSourceSkeleton();
  const ozz::unique_ptr<ozz::animation::Skeleton> target =
      BuildTargetSkeleton();
  if (!source || !target) {
    return EXIT_FAILURE;
  }

  const RawAnimation input = BuildSourceAnimation();

  // Maps every target joint to a source joint by index. -1 would mean "not
  // driven". The extra target intermediate joint (target_neck) is driven by the
  // source spine as well.
  const int source_joint_indices[] = {0,  // target_root <- source_root
                                      1,  // target_spine <- source_spine
                                      1,  // target_neck <- source_spine
                                      2}; // target_head <- source_head

  // Probe times at which the reference pose is evaluated and error is
  // constrained. Must be strictly increasing, start at 0 and end at duration.
  const float probe_times[] = {0.f, .1f, .2f, .3f, .4f, .5f,
                               .6f, .7f, .8f, .9f, 1.f};

  RawAnimationRetargeter::Params params;
  params.source_joint_indices = ozz::make_span(source_joint_indices);
  params.probe_times = ozz::make_span(probe_times);
  params.position_tolerance = 1e-2f;
  params.angle_tolerance = 1e-2f;
  params.root_scale = 1.f;

  RawAnimation output;
  if (!RawAnimationRetargeter()(input, *source, *target, params, &output)) {
    std::printf("Retargeting failed.\n");
    return EXIT_FAILURE;
  }

  // The result is a valid RawAnimation that can be given to AnimationBuilder.
  const ozz::unique_ptr<ozz::animation::Animation> animation =
      ozz::animation::offline::AnimationBuilder()(output);
  if (!animation) {
    return EXIT_FAILURE;
  }

  // Samples the retargeted animation and computes model-space matrices.
  ozz::animation::SamplingJob::Context context(target->num_joints());
  ozz::vector<ozz::math::SoaTransform> locals(target->num_soa_joints());
  ozz::vector<ozz::math::Float4x4> models(target->num_joints());

  ozz::animation::SamplingJob sampling_job;
  sampling_job.animation = animation.get();
  sampling_job.context = &context;
  sampling_job.ratio = .5f;
  sampling_job.output = ozz::make_span(locals);

  ozz::animation::LocalToModelJob local_to_model_job;
  local_to_model_job.skeleton = target.get();
  local_to_model_job.input = ozz::make_span(locals);
  local_to_model_job.output = ozz::make_span(models);

  if (!sampling_job.Run() || !local_to_model_job.Run()) {
    return EXIT_FAILURE;
  }

  std::printf(
      "Retargeted animation: %d tracks, %zu shared key times, "
      "head model origin (%.4f, %.4f, %.4f).\n",
      output.num_tracks(), output.tracks[0].rotations.size(),
      models[target->num_joints() - 1].cols[3].x,
      models[target->num_joints() - 1].cols[3].y,
      models[target->num_joints() - 1].cols[3].z);
  return EXIT_SUCCESS;
}
