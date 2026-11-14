#include <algorithm>
#include <cmath>
#include <cstdio>
#include "ozz/animation/offline/raw_skeleton.h"
#include "ozz/animation/offline/skeleton_builder.h"
#include "ozz/animation/runtime/skeleton.h"
#include "ozz/animation/offline/raw_animation.h"
#include "ozz/animation/offline/raw_animation_utils.h"
#include "ozz/base/maths/simd_math.h"
#include "ozz/geometry/runtime/skinning_job.h"
int main() {
  using namespace ozz;
  using namespace ozz::animation::offline;
  RawSkeleton raw;
  raw.roots.resize(1);
  auto& root=raw.roots[0]; root.name="root";root.transform=math::Transform::identity();
  root.children.resize(1);auto& helper=root.children[0];helper.name="helper";helper.transform=math::Transform::identity();
  helper.children.resize(1);auto& tip=helper.children[0];tip.name="tip";tip.transform=math::Transform::identity();tip.transform.translation=math::Float3(1,0,0);
  auto skeleton=SkeletonBuilder()(raw);
  if(!skeleton||skeleton->num_joints()!=3) return 1;
  RawAnimation clip;clip.duration=1;clip.tracks.resize(3);
  clip.tracks[0].scales.push_back({0,math::Float3(2,1,1)});
  clip.tracks[1].rotations.push_back({0,math::Quaternion::identity()});
  clip.tracks[1].rotations.push_back({1,math::Quaternion::FromAxisAngle(math::Float3::z_axis(),1.57079632679f)});
  clip.tracks[2].translations.push_back({0,math::Float3(1,0,0)});
  math::Transform poses[3];if(!SampleAnimation(clip,.5f,make_span(poses)))return 2;
  math::Float4x4 world[3];world[0]=math::Float4x4::FromAffine(poses[0]);
  world[1]=world[0]*math::Float4x4::FromAffine(poses[1]);world[2]=world[1]*math::Float4x4::FromAffine(poses[2]);
  math::Transform decomposed;
  bool decomposes=math::ToAffine(world[1],&decomposed);
  float residual=0;
  if(decomposes){auto rebuilt=math::Float4x4::FromAffine(decomposed);for(int c=0;c<4;++c){float a[4],b[4];math::StorePtrU(world[1].cols[c],a);math::StorePtrU(rebuilt.cols[c],b);for(int r=0;r<4;++r)residual=std::max(residual,std::abs(a[r]-b[r]));}}
  float input[3]={0,0,0},output[3]={0,0,0};uint16_t slot[1]={0};math::Float4x4 palette[1]={world[2]};
  geometry::SkinningJob skin;skin.vertex_count=1;skin.influences_count=1;skin.joint_matrices=make_span(palette);skin.joint_indices=make_span(slot);skin.joint_indices_stride=sizeof(uint16_t);skin.in_positions=make_span(input);skin.in_positions_stride=sizeof(input);skin.out_positions=make_span(output);skin.out_positions_stride=sizeof(output);
  if(!skin.Run())return 3;
  std::printf("BASELINE joints=%d sampled=0.5 skin=(%.6f,%.6f,%.6f)\n",skeleton->num_joints(),output[0],output[1],output[2]);
  std::printf("CONTRACTION_WITNESS ToAffine=%d recomposition_error=%.6f reducer=absent\n",decomposes,residual);
  return std::abs(output[0]-1.41421356f)<1e-4f && std::abs(output[1]-.70710678f)<1e-4f && (!decomposes||residual>.1f) ? 0:4;
}
