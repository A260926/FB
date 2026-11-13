#include <ozz/geometry/runtime/skinning_job.h>
#include <ozz/base/maths/simd_math.h>
#include <ozz/base/span.h>
#include <cmath>
#include <cstdio>
int main(){
 using namespace ozz; using namespace ozz::math;
 Float4x4 m[2]={Float4x4::identity(),Float4x4::identity()};
 m[0].cols[0]=simd_float4::Load(0,1,0,0); m[0].cols[1]=simd_float4::Load(-1,0,0,0);
 m[1].cols[0]=simd_float4::Load(0,-1,0,0);m[1].cols[1]=simd_float4::Load(1,0,0,0);
 uint16_t indices[]={0,1};float weights[]={.5f};float in[]={1,0,0},out[]={9,9,9};
 geometry::SkinningJob j;j.vertex_count=1;j.influences_count=2;j.joint_matrices=make_span(m);
 j.joint_indices=make_span(indices);j.joint_indices_stride=4;j.joint_weights=make_span(weights);j.joint_weights_stride=4;
 j.in_positions=make_span(in);j.in_positions_stride=12;j.out_positions=make_span(out);j.out_positions_stride=12;
 if(!j.Run() || std::fabs(out[0])+std::fabs(out[1])+std::fabs(out[2])>1e-6f)return 1;
 std::printf("OZZ_LBS_BASELINE_OK radius=%.6f influences=2\n",std::sqrt(out[0]*out[0]+out[1]*out[1]));
 return 0;
}
