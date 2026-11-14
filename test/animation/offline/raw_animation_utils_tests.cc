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

#include "gtest/gtest.h"
#include "ozz/animation/offline/raw_animation.h"
#include "ozz/animation/offline/raw_animation_utils.h"
#include "ozz/animation/offline/raw_skeleton.h"
#include "ozz/animation/offline/skeleton_builder.h"
#include "ozz/animation/runtime/skeleton.h"
#include "ozz/base/gtest_helper.h"
#include "ozz/base/maths/gtest_math_helper.h"

using ozz::animation::offline::RawAnimation;

TEST(SamplingTrackEmpty, Utils) {
  RawAnimation::JointTrack track;
  ozz::math::Transform output;

  ASSERT_TRUE(ozz::animation::offline::SampleTrack(track, 0.f, &output));

  EXPECT_FLOAT3_EQ(output.translation, 0.f, 0.f, 0.f);
  EXPECT_QUATERNION_EQ(output.rotation, 0.f, 0.f, 0.f, 1.f);
  EXPECT_FLOAT3_EQ(output.scale, 1.f, 1.f, 1.f);
}

TEST(SamplingTrackInvalid, Utils) {
  // Key order
  {
    RawAnimation::JointTrack track;

    RawAnimation::TranslationKey t0 = {.9f, ozz::math::Float3(1.f, 2.f, 4.f)};
    track.translations.push_back(t0);
    RawAnimation::TranslationKey t1 = {.1f, ozz::math::Float3(2.f, 4.f, 8.f)};
    track.translations.push_back(t1);

    ozz::math::Transform output;
    EXPECT_FALSE(ozz::animation::offline::SampleTrack(track, 0.f, &output));
  }

  // Negative time
  {
    RawAnimation::JointTrack track;

    RawAnimation::TranslationKey t0 = {-1.f, ozz::math::Float3(1.f, 2.f, 4.f)};
    track.translations.push_back(t0);

    ozz::math::Transform output;
    EXPECT_FALSE(ozz::animation::offline::SampleTrack(track, 0.f, &output));
  }
}

TEST(SamplingTrack, Utils) {
  RawAnimation::JointTrack track;

  track.translations.push_back({.1f, ozz::math::Float3(1.f, 2.f, 4.f)});
  track.translations.push_back({.9f, ozz::math::Float3(2.f, 4.f, 8.f)});

  track.rotations.push_back(
      {0.f, ozz::math::Quaternion(.70710677f, 0.f, 0.f, .70710677f)});
  track.rotations.push_back(
      {// /!\ Negated (other hemisphepre) quaternion
       .5f, -ozz::math::Quaternion(0.f, .70710677f, 0.f, .70710677f)});
  track.rotations.push_back(
      {1.f, ozz::math::Quaternion(0.f, .70710677f, 0.f, .70710677f)});

  track.scales.push_back({.5f, ozz::math::Float3(-1.f, -2.f, -4.f)});

  ozz::math::Transform output;

  // t = -.1
  ASSERT_TRUE(ozz::animation::offline::SampleTrack(track, -.1f, &output));
  EXPECT_FLOAT3_EQ(output.translation, 1.f, 2.f, 4.f);
  EXPECT_QUATERNION_EQ(output.rotation, .70710677f, 0.f, 0.f, .70710677f);
  EXPECT_FLOAT3_EQ(output.scale, -1.f, -2.f, -4.f);

  // t = 0
  ASSERT_TRUE(ozz::animation::offline::SampleTrack(track, 0.f, &output));
  EXPECT_FLOAT3_EQ(output.translation, 1.f, 2.f, 4.f);
  EXPECT_QUATERNION_EQ(output.rotation, .70710677f, 0.f, 0.f, .70710677f);
  EXPECT_FLOAT3_EQ(output.scale, -1.f, -2.f, -4.f);

  // t = .1
  ASSERT_TRUE(ozz::animation::offline::SampleTrack(track, .1f, &output));
  EXPECT_FLOAT3_EQ(output.translation, 1.f, 2.f, 4.f);
  EXPECT_QUATERNION_EQ(output.rotation, .6172133f, .1543033f, 0.f, .7715167f);
  EXPECT_FLOAT3_EQ(output.scale, -1.f, -2.f, -4.f);

  // t = .4999999
  ASSERT_TRUE(ozz::animation::offline::SampleTrack(track, .4999999f, &output));
  EXPECT_FLOAT3_EQ(output.translation, 1.5f, 3.f, 6.f);
  EXPECT_QUATERNION_EQ(output.rotation, 0.f, .70710677f, 0.f, .70710677f);
  EXPECT_FLOAT3_EQ(output.scale, -1.f, -2.f, -4.f);

  // t = .5
  ASSERT_TRUE(ozz::animation::offline::SampleTrack(track, .5f, &output));
  EXPECT_FLOAT3_EQ(output.translation, 1.5f, 3.f, 6.f);
  EXPECT_QUATERNION_EQ(output.rotation, 0.f, .70710677f, 0.f, .70710677f);
  EXPECT_FLOAT3_EQ(output.scale, -1.f, -2.f, -4.f);

  // t = .75
  ASSERT_TRUE(ozz::animation::offline::SampleTrack(track, .75f, &output));
  // Fixed up based on dot with previous quaternion
  EXPECT_QUATERNION_EQ(output.rotation, 0.f, -.70710677f, 0.f, -.70710677f);
  EXPECT_FLOAT3_EQ(output.scale, -1.f, -2.f, -4.f);

  // t= .9
  ASSERT_TRUE(ozz::animation::offline::SampleTrack(track, .9f, &output));
  EXPECT_FLOAT3_EQ(output.translation, 2.f, 4.f, 8.f);
  EXPECT_QUATERNION_EQ(output.rotation, 0.f, -.70710677f, 0.f, -.70710677f);
  EXPECT_FLOAT3_EQ(output.scale, -1.f, -2.f, -4.f);

  // t= 1.
  ASSERT_TRUE(ozz::animation::offline::SampleTrack(track, 1.f, &output));
  EXPECT_FLOAT3_EQ(output.translation, 2.f, 4.f, 8.f);
  EXPECT_QUATERNION_EQ(output.rotation, 0.f, .70710677f, 0.f, .70710677f);
  EXPECT_FLOAT3_EQ(output.scale, -1.f, -2.f, -4.f);

  // t= 1.1
  ASSERT_TRUE(ozz::animation::offline::SampleTrack(track, 1.1f, &output));
  EXPECT_FLOAT3_EQ(output.translation, 2.f, 4.f, 8.f);
  EXPECT_QUATERNION_EQ(output.rotation, 0.f, .70710677f, 0.f, .70710677f);
  EXPECT_FLOAT3_EQ(output.scale, -1.f, -2.f, -4.f);
}

TEST(SamplingTrackModelSpace, Utils) {
  /*
   3 joints

     *
     |
    j0
    / \
   j1 j2
  */

  ozz::animation::offline::RawSkeleton raw_skeleton;
  raw_skeleton.roots.resize(1);
  auto& root = raw_skeleton.roots[0];
  root.children.resize(2);
  EXPECT_TRUE(raw_skeleton.Validate());
  EXPECT_EQ(raw_skeleton.num_joints(), 3);

  auto skeleton = ozz::animation::offline::SkeletonBuilder()(raw_skeleton);
  EXPECT_EQ(skeleton->num_joints(), 3);

  RawAnimation animation;
  animation.duration = 1.f;
  animation.tracks.resize(3);

  // build track
  {
    auto& track = animation.tracks[0];
    track.translations.push_back({0.f, ozz::math::Float3(1.f, 0.f, 0.f)});
    track.rotations.push_back(
        {0.f, ozz::math::Quaternion::FromAxisAngle(ozz::math::Float3::y_axis(),
                                                   ozz::math::kPi_2)});
    track.scales.push_back({0.f, ozz::math::Float3(2.f, 2.f, 2.f)});
  }
  {
    auto& track = animation.tracks[1];
    track.translations.push_back({0.f, ozz::math::Float3(0.f, 1.f, 0.f)});
  }
  {
    auto& track = animation.tracks[2];
    track.translations.push_back({0.f, ozz::math::Float3(0.f, 0.f, 0.f)});
    track.translations.push_back({.1f, ozz::math::Float3(0.f, 0.f, 1.f)});

    track.rotations.push_back(
        {.3f, ozz::math::Quaternion::FromAxisAngle(ozz::math::Float3::y_axis(),
                                                   -ozz::math::kPi)});
  }

  {  // Invalid joint index (out of range)
    const auto [valid, output] =
        ozz::animation::offline::SampleTrackModelSpace(animation, *skeleton, 3);
    EXPECT_FALSE(valid);
    EXPECT_TRUE(output.empty());
  }
  {  // Invalid joint index (negative)
    const auto [valid, output] = ozz::animation::offline::SampleTrackModelSpace(
        animation, *skeleton, -1);
    EXPECT_FALSE(valid);
    EXPECT_TRUE(output.empty());
  }

  {  // Sample root
    const auto [valid, output] =
        ozz::animation::offline::SampleTrackModelSpace(animation, *skeleton, 0);
    ASSERT_TRUE(valid);
    ASSERT_EQ(output.size(), 1u);
    EXPECT_FLOAT_EQ(output[0].first, 0.f);
    EXPECT_FLOAT4x4_EQ(output[0].second, 0.f, 0.f, -2.f, 0.f, 0.f, 2.f, 0.f,
                       0.f, 2.f, 0.f, 0.f, 0.f, 1.f, 0.f, 0.f, 1.f);
  }

  {  // Sample child
    const auto [valid, output] =
        ozz::animation::offline::SampleTrackModelSpace(animation, *skeleton, 2);
    ASSERT_TRUE(valid);
    ASSERT_EQ(output.size(), 3u);

    EXPECT_FLOAT_EQ(output[0].first, 0.f);
    EXPECT_FLOAT4x4_EQ(output[0].second, 0.f, 0.f, 2.f, 0.f, 0.f, 2.f, 0.f, 0.f,
                       -2.f, 0.f, 0.f, 0.f, 1.f, 0.f, 0.f, 1.f);

    EXPECT_FLOAT_EQ(output[1].first, .1f);
    EXPECT_FLOAT4x4_EQ(output[1].second, 0.f, 0.f, 2.f, 0.f, 0.f, 2.f, 0.f, 0.f,
                       -2.f, 0.f, 0.f, 0.f, 3.f, 0.f, 0.f, 1.f);

    EXPECT_FLOAT_EQ(output[2].first, .3f);
    EXPECT_FLOAT4x4_EQ(output[2].second, 0.f, 0.f, 2.f, 0.f, 0.f, 2.f, 0.f, 0.f,
                       -2.f, 0.f, 0.f, 0.f, 3.f, 0.f, 0.f, 1.f);
  }
}

TEST(SamplingAnimation, Utils) {
  // Building an Animation with unsorted keys fails.
  RawAnimation raw_animation;
  raw_animation.duration = 2.f;
  raw_animation.tracks.resize(2);

  RawAnimation::TranslationKey a = {.2f, ozz::math::Float3(-1.f, 0.f, 0.f)};
  raw_animation.tracks[0].translations.push_back(a);

  RawAnimation::TranslationKey b = {0.f, ozz::math::Float3(2.f, 0.f, 0.f)};
  raw_animation.tracks[1].translations.push_back(b);
  RawAnimation::TranslationKey c = {0.2f, ozz::math::Float3(6.f, 0.f, 0.f)};
  raw_animation.tracks[1].translations.push_back(c);
  RawAnimation::TranslationKey d = {0.4f, ozz::math::Float3(8.f, 0.f, 0.f)};
  raw_animation.tracks[1].translations.push_back(d);

  ozz::math::Transform output[2];

  // Too small
  {
    ozz::math::Transform small[1];
    EXPECT_FALSE(
        ozz::animation::offline::SampleAnimation(raw_animation, 0.f, small));
  }

  // Invalid, cos track are longer than duration
  {
    raw_animation.duration = .1f;
    EXPECT_FALSE(
        ozz::animation::offline::SampleAnimation(raw_animation, 0.f, output));
    raw_animation.duration = 2.f;
  }

  ASSERT_TRUE(
      ozz::animation::offline::SampleAnimation(raw_animation, -.1f, output));
  EXPECT_FLOAT3_EQ(output[0].translation, -1.f, 0.f, 0.f);
  EXPECT_QUATERNION_EQ(output[0].rotation, 0.f, 0.f, 0.f, 1.f);
  EXPECT_FLOAT3_EQ(output[0].scale, 1.f, 1.f, 1.f);
  EXPECT_FLOAT3_EQ(output[1].translation, 2.f, 0.f, 0.f);
  EXPECT_QUATERNION_EQ(output[1].rotation, 0.f, 0.f, 0.f, 1.f);
  EXPECT_FLOAT3_EQ(output[1].scale, 1.f, 1.f, 1.f);

  ASSERT_TRUE(
      ozz::animation::offline::SampleAnimation(raw_animation, 0.f, output));
  EXPECT_FLOAT3_EQ(output[0].translation, -1.f, 0.f, 0.f);
  EXPECT_FLOAT3_EQ(output[1].translation, 2.f, 0.f, 0.f);

  ASSERT_TRUE(
      ozz::animation::offline::SampleAnimation(raw_animation, .2f, output));
  EXPECT_FLOAT3_EQ(output[0].translation, -1.f, 0.f, 0.f);
  EXPECT_FLOAT3_EQ(output[1].translation, 6.f, 0.f, 0.f);

  ASSERT_TRUE(
      ozz::animation::offline::SampleAnimation(raw_animation, .3f, output));
  EXPECT_FLOAT3_EQ(output[0].translation, -1.f, 0.f, 0.f);
  EXPECT_FLOAT3_EQ(output[1].translation, 7.f, 0.f, 0.f);

  ASSERT_TRUE(
      ozz::animation::offline::SampleAnimation(raw_animation, .4f, output));
  EXPECT_FLOAT3_EQ(output[0].translation, -1.f, 0.f, 0.f);
  EXPECT_FLOAT3_EQ(output[1].translation, 8.f, 0.f, 0.f);

  ASSERT_TRUE(
      ozz::animation::offline::SampleAnimation(raw_animation, 2.f, output));
  EXPECT_FLOAT3_EQ(output[0].translation, -1.f, 0.f, 0.f);
  EXPECT_FLOAT3_EQ(output[1].translation, 8.f, 0.f, 0.f);

  ASSERT_TRUE(
      ozz::animation::offline::SampleAnimation(raw_animation, 3.f, output));
  EXPECT_FLOAT3_EQ(output[0].translation, -1.f, 0.f, 0.f);
  EXPECT_FLOAT3_EQ(output[1].translation, 8.f, 0.f, 0.f);

  {  // Bigger output set to identity
    ozz::math::Transform bigger[3];
    ASSERT_TRUE(
        ozz::animation::offline::SampleAnimation(raw_animation, 0.f, bigger));
    EXPECT_FLOAT3_EQ(bigger[0].translation, -1.f, 0.f, 0.f);
    EXPECT_FLOAT3_EQ(bigger[1].translation, 2.f, 0.f, 0.f);
    EXPECT_FLOAT3_EQ(bigger[2].translation, 0.f, 0.f, 0.f);
    EXPECT_QUATERNION_EQ(bigger[2].rotation, 0.f, 0.f, 0.f, 1.f);
    EXPECT_FLOAT3_EQ(bigger[2].scale, 1.f, 1.f, 1.f);
  }
}

TEST(FixedRateSamplingTime, Utils) {
  {  // From 0
    ozz::animation::offline::FixedRateSamplingTime it(1.f, 30.f);
    EXPECT_EQ(it.num_keys(), 31u);

    EXPECT_EQ(it.time(0), 0.f);
    EXPECT_FLOAT_EQ(it.time(1), 1.f / 30.f);
    EXPECT_FLOAT_EQ(it.time(2), 2.f / 30.f);
    EXPECT_FLOAT_EQ(it.time(29), 29.f / 30.f);
    EXPECT_EQ(it.time(30), 1.f);
    EXPECT_ASSERTION(it.time(31), "_key < num_keys");
  }

  {  // Offset
    ozz::animation::offline::FixedRateSamplingTime it(3.f, 100.f);
    EXPECT_EQ(it.num_keys(), 301u);

    EXPECT_EQ(it.time(0), 0.f);
    EXPECT_FLOAT_EQ(it.time(1), 1.f / 100.f);
    EXPECT_FLOAT_EQ(it.time(2), 2.f / 100.f);
    EXPECT_FLOAT_EQ(it.time(299), 299.f / 100.f);
    EXPECT_EQ(it.time(300), 3.f);
  }

  {  // Ceil
    ozz::animation::offline::FixedRateSamplingTime it(1.001f, 30.f);
    EXPECT_EQ(it.num_keys(), 32u);

    EXPECT_EQ(it.time(0), 0.f);
    EXPECT_FLOAT_EQ(it.time(1), 1.f / 30.f);
    EXPECT_FLOAT_EQ(it.time(2), 2.f / 30.f);
    EXPECT_FLOAT_EQ(it.time(29), 29.f / 30.f);
    EXPECT_FLOAT_EQ(it.time(30), 1.f);
    EXPECT_EQ(it.time(31), 1.001f);
  }

  {  // Long
    ozz::animation::offline::FixedRateSamplingTime it(1000.f, 30.f);
    EXPECT_EQ(it.num_keys(), 30001u);

    EXPECT_EQ(it.time(0), 0.f);
    EXPECT_FLOAT_EQ(it.time(1), 1.f / 30.f);
    EXPECT_FLOAT_EQ(it.time(2), 2.f / 30.f);
    EXPECT_FLOAT_EQ(it.time(29999), 29999.f / 30.f);
    EXPECT_EQ(it.time(30000), 1000.f);
  }
}

TEST(TimePoints, Utils) {
  // Building an Animation with unsorted keys fails.
  RawAnimation raw_animation;
  raw_animation.tracks.resize(3);

  raw_animation.tracks[0].translations.push_back({0.f, {}});
  raw_animation.tracks[0].translations.push_back({.2f, {}});
  raw_animation.tracks[0].translations.push_back({.4f, {}});
  raw_animation.tracks[0].translations.push_back({2.f, {}});
  raw_animation.tracks[0].rotations.push_back({0.f, {}});
  raw_animation.tracks[0].rotations.push_back({.1f, {}});
  raw_animation.tracks[0].rotations.push_back({.3f, {}});
  raw_animation.tracks[0].scales.push_back({0.f, {}});
  raw_animation.tracks[0].scales.push_back({.1f, {}});
  raw_animation.tracks[0].scales.push_back({.3f, {}});

  raw_animation.tracks[1].translations.push_back({0.f, {}});
  raw_animation.tracks[1].translations.push_back({.1f, {}});
  raw_animation.tracks[1].translations.push_back({.6f, {}});
  raw_animation.tracks[1].scales.push_back({1.f, {}});

  raw_animation.tracks[2].translations.push_back({.2f, {}});
  raw_animation.tracks[2].translations.push_back({.4f, {}});
  raw_animation.tracks[2].translations.push_back({.5f, {}});
  raw_animation.tracks[2].rotations.push_back({.7f, {}});

  {  // Invalid
    raw_animation.duration = 1.f;
    EXPECT_FALSE(raw_animation.Validate());
    const auto [valid, time_points] = ExtractTimePoints(raw_animation);
    ASSERT_FALSE(valid);
    EXPECT_TRUE(time_points.empty());
  }

  {  // Valid, all joints
    raw_animation.duration = 2.f;
    const auto [valid, time_points] = ExtractTimePoints(raw_animation);
    ASSERT_TRUE(valid);
    ASSERT_EQ(time_points.size(), 10u);
    EXPECT_FLOAT_EQ(time_points[0], 0.f);
    EXPECT_FLOAT_EQ(time_points[1], .1f);
    EXPECT_FLOAT_EQ(time_points[2], .2f);
    EXPECT_FLOAT_EQ(time_points[3], .3f);
    EXPECT_FLOAT_EQ(time_points[4], .4f);
    EXPECT_FLOAT_EQ(time_points[5], .5f);
    EXPECT_FLOAT_EQ(time_points[6], .6f);
    EXPECT_FLOAT_EQ(time_points[7], .7f);
    EXPECT_FLOAT_EQ(time_points[8], 1.f);
    EXPECT_FLOAT_EQ(time_points[9], 2.f);
  }

  {  // Valid animation, no joint
    const auto [valid, time_points] =
        ExtractTimePoints(raw_animation, make_span(ozz::vector<int16_t>{}));
    ASSERT_TRUE(valid);
    EXPECT_TRUE(time_points.empty());
  }

  {  // Valid animation, invalid joints
    const auto [valid, time_points] = ExtractTimePoints(
        raw_animation, make_span(ozz::vector<int16_t>{0, -1, 1}));
    ASSERT_FALSE(valid);
    EXPECT_TRUE(time_points.empty());
  }
  {  // Valid animation, invalid joints
    const auto [valid, time_points] = ExtractTimePoints(
        raw_animation, make_span(ozz::vector<int16_t>{0, 5, 1}));
    ASSERT_FALSE(valid);
    EXPECT_TRUE(time_points.empty());
  }

  {  // Valid, specific joints
    const auto [valid, time_points] =
        ExtractTimePoints(raw_animation, make_span(ozz::vector<int16_t>{0, 2}));
    ASSERT_EQ(time_points.size(), 8u);
    EXPECT_FLOAT_EQ(time_points[0], 0.f);
    EXPECT_FLOAT_EQ(time_points[1], .1f);
    EXPECT_FLOAT_EQ(time_points[2], .2f);
    EXPECT_FLOAT_EQ(time_points[3], .3f);
    EXPECT_FLOAT_EQ(time_points[4], .4f);
    EXPECT_FLOAT_EQ(time_points[5], .5f);
    EXPECT_FLOAT_EQ(time_points[6], .7f);
    EXPECT_FLOAT_EQ(time_points[7], 2.f);
  }
}