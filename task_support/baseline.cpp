#include "cal3d/coreskeleton.h"
#include "cal3d/corebone.h"
#include "cal3d/coreanimation.h"
#include "cal3d/corekeyframe.h"
#include "cal3d/animation.h"
#include "cal3d/mixer.h"
#include "cal3d/skeleton.h"
#include "cal3d/bone.h"
#include "cal3d/submesh.h"
#include "cal3d/physique.h"
#include <cmath>
#include <cstdio>
static bool close(float a,float b){return std::fabs(a-b)<2e-5f;}
int main(){
 auto core=std::make_shared<CalCoreSkeleton>();
 core->addCoreBone(std::make_shared<CalCoreBone>("root"));
 core->addCoreBone(std::make_shared<CalCoreBone>("child",0));
 core->coreBones[0]->relativeTransform=cal3d::RotateTranslate();
 core->coreBones[1]->relativeTransform.translation=CalVector(0,1,0);
 core->coreBones[0]->inverseBindPoseTransform.translation=CalVector(-1,0,0);
 core->coreBones[1]->inverseBindPoseTransform.translation=CalVector(-1,-1,0);
 auto clip=std::make_shared<CalCoreAnimation>();clip->duration=1.2f;
 CalCoreTrack::KeyframeList keys;
 keys.push_back(CalCoreKeyframe(0,CalVector(0,0,0),CalQuaternion()));
 keys.push_back(CalCoreKeyframe(.3f,CalVector(3,0,0),CalQuaternion()));
 keys.push_back(CalCoreKeyframe(1.2f,CalVector(12,0,0),CalQuaternion()));
 clip->tracks.push_back(CalCoreTrack(0,keys));
 const float times[]={-1,.3f,.5f,1.2f,2};const float want[]={0,3,5,12,12};
 for(int i=0;i<5;++i)if(!close(clip->tracks[0].getCurrentTransform(times[i]).translation.x,want[i]))return 2;
 CalCoreTrack empty(1,{});if(!close(empty.getCurrentTransform(.5f).translation.x,0))return 3;
 std::puts("BASELINE_TRACK_OK samples=5 empty_identity=1");
 auto low=std::make_shared<CalAnimation>(clip,1,0);low->time=.5f;
 auto highClip=std::make_shared<CalCoreAnimation>();highClip->duration=1;
 highClip->tracks.push_back(CalCoreTrack(0,{CalCoreKeyframe(0,CalVector(-2,0,0),CalQuaternion())}));
 auto high=std::make_shared<CalAnimation>(highClip,1,1);high->rampValue=.5f;
 CalMixer mixer;mixer.addAnimation(low);mixer.addAnimation(high);CalSkeleton skeleton(core);
 mixer.updateSkeleton(&skeleton,{},{});
 if(!close(skeleton.bones[0].absoluteTransform.translation.x,1.5f)||!close(skeleton.bones[1].absoluteTransform.translation.y,1))return 4;
 auto sub=std::make_shared<CalCoreSubmesh>(1,false,0);
 CalCoreSubmesh::Vertex v={CalPoint4(2,1,0),CalVector4(0,1,0)};
 sub->addVertex(v,CalColor32(0),{CalCoreSubmesh::Influence(0,.25f,false),CalCoreSubmesh::Influence(1,.75f,true)});
 CalSubmesh runtime(sub);CalVector4 out[2];
 CalPhysique::calculateVerticesAndNormals(cal3d::pointerFromVector(skeleton.boneTransforms),&runtime,reinterpret_cast<float*>(out));
 if(!close(out[0].x,2.5f)||!close(out[0].y,1)||!close(out[1].y,1))return 5;
 std::printf("BASELINE_MIX_SKIN_OK x=%.3f y=%.3f priority=1 inverse_bind=1\n",out[0].x,out[0].y);
 mixer.removeAnimation(high);mixer.updateSkeleton(&skeleton,{},{});
 CalPhysique::calculateVerticesAndNormals(cal3d::pointerFromVector(skeleton.boneTransforms),&runtime,reinterpret_cast<float*>(out));
 if(!close(out[0].x,6))return 6;
 std::puts("BASELINE_REMOVE_OK x=6.000");
}
