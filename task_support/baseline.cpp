#include <cstdio>
#include <cmath>
#include "ozz/animation/offline/raw_skeleton.h"
#include "ozz/animation/offline/skeleton_builder.h"
#include "ozz/animation/runtime/skeleton.h"
#include "ozz/animation/runtime/local_to_model_job.h"
#include "ozz/animation/runtime/ik_aim_job.h"
#include "ozz/base/maths/soa_transform.h"
#include "ozz/base/maths/simd_quaternion.h"
int main(){
 using namespace ozz;using namespace ozz::animation;using namespace ozz::math;
 offline::RawSkeleton raw;raw.roots.resize(1);auto& root=raw.roots[0];
 root.children.resize(2);
 for(int i=0;i<2;++i){auto& arm=root.children[i];arm.transform.translation=Float3(float(i? -1:1),0,0);arm.children.resize(1);arm.children[0].transform.translation=arm.transform.translation;}
 offline::SkeletonBuilder build;auto sk=build(raw);if(!sk || sk->num_joints()!=5)return 1;
 Float4x4 out[5];LocalToModelJob fk;fk.skeleton=sk.get();fk.input=sk->joint_rest_poses();fk.output=make_span(out);if(!fk.Run())return 2;
 if(std::abs(GetX(out[2].cols[3])-2)>1e-6 || std::abs(GetX(out[4].cols[3])+2)>1e-6)return 3;
 SimdQuaternion correction;IKAimJob aim;aim.joint=&out[0];aim.target=simd_float4::Load(2,0,0,0);aim.joint_correction=&correction;
 if(!aim.Run() || std::abs(GetW(correction.xyzw)-1)>1e-5)return 4;
 std::puts("BASELINE_OK joints=5 fk_endpoints=2,-2 aim=identity");
}
