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
#include "ozz/base/gtest_helper.h"
#include "ozz/base/maths/gtest_math_helper.h"
#include "ozz/base/maths/transform.h"

using ozz::math::Transform;

TEST(TransformDefault, ozz_math) {
  EXPECT_FLOAT3_EQ(Transform{}.translation, 0.f, 0.f, 0.f);
  EXPECT_QUATERNION_EQ(Transform{}.rotation, 0.f, 0.f, 0.f, 1.f);
  EXPECT_FLOAT3_EQ(Transform{}.scale, 1.f, 1.f, 1.f);
}

TEST(TransformConstant, ozz_math) {
  EXPECT_FLOAT3_EQ(Transform::identity().translation, 0.f, 0.f, 0.f);
  EXPECT_QUATERNION_EQ(Transform::identity().rotation, 0.f, 0.f, 0.f, 1.f);
  EXPECT_FLOAT3_EQ(Transform::identity().scale, 1.f, 1.f, 1.f);
}

TEST(TransformOperation, ozz_math) {
  const Transform t0{ozz::math::Float3(1.f, 0.f, 0.f),
                     ozz::math::Quaternion::FromAxisAngle(
                         ozz::math::Float3::y_axis(), ozz::math::kPi_2),
                     ozz::math::Float3(2.f, 2.f, 2.f)};
  const Transform t1{ozz::math::Float3(0.f, 1.f, 1.f),
                     ozz::math::Quaternion::FromAxisAngle(
                         ozz::math::Float3::x_axis(), ozz::math::kPi_2),
                     ozz::math::Float3(3.f, 3.f, 3.f)};

  const auto tm = t0 * t1;
  EXPECT_FLOAT3_EQ(tm.translation, 3.f, 2.f, 0.f);
  EXPECT_QUATERNION_EQ(tm.rotation, .5f, .5f, -.5f, .5f);
  EXPECT_FLOAT3_EQ(tm.scale, 6.f, 6.f, 6.f);
}
