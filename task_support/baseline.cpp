#include <cal3d/corebone.h>
#include <cal3d/coreskeleton.h>
#include <cal3d/skeleton.h>
#include <cal3d/mixer.h>
#include <cal3d/coreanimation.h>
#include <cal3d/submesh.h>
#include <cal3d/physique.h>
#include <cal3d/coremorphtarget.h>
#include <cmath>
#include <cstdio>
#include <stdexcept>
static void need(bool v){if(!v)throw std::runtime_error("baseline mismatch");}
int main(){
 auto core=std::make_shared<CalCoreSkeleton>();auto bone=std::make_shared<CalCoreBone>("root");core->addCoreBone(bone);
 CalSkeleton sk(core);CalMixer mixer;
 auto clip=std::make_shared<CalCoreAnimation>();clip->duration=1;
 CalCoreTrack::KeyframeList keys;keys.push_back(CalCoreKeyframe(0,CalVector(0,0,0),CalQuaternion()));keys.push_back(CalCoreKeyframe(1,CalVector(2,0,0),CalQuaternion()));
 clip->tracks.push_back(CalCoreTrack(0,keys));auto anim=std::make_shared<CalAnimation>(clip,1,0);anim->time=.5f;anim->rampValue=1;mixer.addAnimation(anim);
 mixer.updateSkeleton(&sk,{},{});
 auto cm=std::make_shared<CalCoreSubmesh>(3,false,1);
 const CalPoint4 pts[]={CalPoint4(0,0,0),CalPoint4(1,0,0),CalPoint4(0,1,0)};
 for(auto p:pts){CalCoreSubmesh::Vertex v={p,CalVector4(0,0,1)};cm->addVertex(v,0xffffffff,{{0,1,true}});}cm->addFace({0,1,2});
 CalCoreMorphTarget::VertexOffsetArray offsets;offsets.push_back(VertexOffset(1,CalPoint4(0,0,.5f),CalVector4(0,0,0)));
 cm->addMorphTarget(std::make_shared<CalCoreMorphTarget>("lift.additive",3,offsets));CalSubmesh sm(cm);sm.setMorphTargetWeight("lift.additive",.5f);
 alignas(16) float out[24]={};CalPhysique::calculateVerticesAndNormals(sk.boneTransforms.data(),&sm,out);
 need(std::fabs(out[0]-1)<1e-6);need(std::fabs(out[8]-2)<1e-6);need(std::fabs(out[10]-.25f)<1e-6);need(std::fabs(out[6]-1)<1e-6);
 std::printf("CAL_LBS_MORPH_OK root_x=%.3f vertex1_z=%.3f\n",out[0],out[10]);
}
