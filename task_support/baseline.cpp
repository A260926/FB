#include <cassert>
#include <cmath>
#include <cstdio>
#include "ozz/animation/offline/raw_skeleton.h"
#include "ozz/animation/offline/skeleton_builder.h"
#include "ozz/animation/offline/raw_animation.h"
#include "ozz/animation/offline/raw_animation_utils.h"
#include "ozz/animation/offline/animation_builder.h"
#include "ozz/animation/runtime/animation.h"
#include "ozz/animation/runtime/sampling_job.h"
#include "ozz/animation/runtime/local_to_model_job.h"
#include "ozz/base/maths/soa_transform.h"
#include "ozz/base/maths/simd_math.h"
#include "ozz/base/maths/transform.h"
#include "ozz/base/containers/vector.h"
int main(){
 using namespace ozz; using namespace animation; using namespace offline;
 RawSkeleton raw; raw.roots.resize(1); auto* joint=&raw.roots[0];
 for(int i=0;i<5;++i){joint->name="joint"; joint->transform=math::Transform::identity();joint->transform.translation=math::Float3(0,1,0);if(i<4){joint->children.resize(1);joint=&joint->children[0];}}
 auto skeleton=SkeletonBuilder()(raw);assert(skeleton && skeleton->num_joints()==5);
 RawAnimation a;a.duration=1;a.tracks.resize(5);
 for(auto& t:a.tracks)t.translations.push_back({0,math::Float3(0,1,0)});
 a.tracks[0].translations.push_back({1,math::Float3(2,1,0)});
 vector<math::Transform> sampled(5);assert(SampleAnimation(a,.5f,make_span(sampled)));assert(std::fabs(sampled[0].translation.x-1)<1e-6);
 auto clip=AnimationBuilder()(a);assert(clip);
 SamplingJob::Context context(5);vector<math::SoaTransform> locals(2);SamplingJob sample;sample.animation=clip.get();sample.context=&context;sample.ratio=.5f;sample.output=make_span(locals);assert(sample.Run());
 vector<math::Float4x4> models(5);LocalToModelJob fk;fk.skeleton=skeleton.get();fk.input=make_span(locals);fk.output=make_span(models);assert(fk.Run());
 float p[4];math::StorePtrU(models[4].cols[3],p);assert(std::fabs(p[0]-1)<1e-3 && std::fabs(p[1]-5)<1e-3);
 std::printf("OZZ_BASELINE joints=5 sample_x=%.6f end_y=%.6f\n",p[0],p[1]);
}
