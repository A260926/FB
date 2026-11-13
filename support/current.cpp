#include <Jolt/Jolt.h>
#include <Jolt/Core/Memory.h>
#include <Jolt/Skeleton/Skeleton.h>
#include <Jolt/Skeleton/SkeletonPose.h>
#include <Jolt/Skeleton/SkeletalAnimation.h>
#include <cmath>
#include <cstdio>
#include <cstdlib>
using namespace JPH;
static void require(bool ok, const char *msg) { if (!ok) { std::fprintf(stderr,"FAIL %s\n",msg); std::exit(1); } }
int main() {
 RegisterDefaultAllocator();
 Ref<Skeleton> sk = new Skeleton;
 sk->AddJoint("root", -1); sk->AddJoint("hand", 0);
 SkeletalAnimation clip; clip.SetIsLooping(false);
 auto &tracks = clip.GetAnimatedJoints(); tracks.resize(2);
 tracks[0].mJointName = "root"; tracks[1].mJointName = "hand";
 for (int j=0;j<2;++j) for (int k=0;k<2;++k) {
  SkeletalAnimation::Keyframe key; key.mTime = float(k);
  if (j==0) key.mRotation=Quat::sRotation(Vec3::sAxisZ(), k*JPH_PI/2);
  else key.mTranslation=Vec3(2,0,0);
  tracks[j].mKeyframes.push_back(key);
 }
 SkeletonPose pose; pose.SetSkeleton(sk);
 clip.Sample(0.5f,pose); pose.CalculateJointMatrices();
 Vec3 tip=pose.GetJointMatrix(1)*Vec3(1,0,0);
 require(std::abs(tip.GetX()-3/std::sqrt(2.f))<2e-5f,"ancestor rotation x");
 require(std::abs(tip.GetY()-3/std::sqrt(2.f))<2e-5f,"ancestor rotation y");
 clip.Sample(1.f,pose); pose.CalculateJointMatrices();
 require((pose.GetJointMatrix(1)*Vec3(1,0,0)-Vec3(0,3,0)).Length()<2e-5f,"nonloop endpoint");
 clip.SetIsLooping(true); clip.Sample(1.f,pose); pose.CalculateJointMatrices();
 require((pose.GetJointMatrix(1)*Vec3(1,0,0)-Vec3(3,0,0)).Length()<2e-5f,"loop wraps endpoint");
 std::puts("JOLT_SKELETON_BASELINE_OK ancestor_slerp endpoint loop");
}
