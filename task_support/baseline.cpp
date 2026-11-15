#include "cal3d/coreskeleton.h"
#include "cal3d/corebone.h"
#include "cal3d/coreanimation.h"
#include "cal3d/coretrack.h"
#include "cal3d/corekeyframe.h"
#include "cal3d/animation.h"
#include "cal3d/mixer.h"
#include "cal3d/skeleton.h"
#include "cal3d/bone.h"
#include "cal3d/coremesh.h"
#include "cal3d/coresubmesh.h"
#include "cal3d/coremorphtarget.h"
#include "cal3d/submesh.h"
#include "cal3d/physique.h"
#include "cal3d/saver.h"
#include "cal3d/loader.h"
#include "cal3d/buffersource.h"
#include <cmath>
#include <cstdio>
static bool near(float a,float b){return std::fabs(a-b)<2e-5f;}
static void forward(const CalSkeleton& skeleton,const CalSubmesh& sub,CalVector4* out){CalPhysique::calculateVerticesAndNormals(cal3d::pointerFromVector(skeleton.boneTransforms),&sub,reinterpret_cast<float*>(out));}
int main(){
 auto core=std::make_shared<CalCoreSkeleton>();
 for(unsigned i=0;i<2;++i){auto b=std::make_shared<CalCoreBone>(i?"child":"root",i?0:-1);b->relativeTransform.translation=CalVector(i?0:1,0,0);b->inverseBindPoseTransform.translation=CalVector(-1,0,0);core->addCoreBone(b);}
 // addCoreBone canonicalizes the initial root; establish the final intended bind reference after insertion.
 core->coreBones[0]->relativeTransform.translation=CalVector(1,0,0);
 CalSkeleton sk(core);CalMixer mixer;
 auto ca=std::make_shared<CalCoreAnimation>();
 CalCoreTrack::KeyframeList keys; keys.push_back(CalCoreKeyframe(0,CalVector(0,0,0),CalQuaternion(0,0,1,0)));ca->tracks.push_back(CalCoreTrack(1,keys));
 auto animation=std::make_shared<CalAnimation>(ca,1,0);mixer.addAnimation(animation);
 mixer.updateSkeleton(&sk,{},{});
 auto sub=std::make_shared<CalCoreSubmesh>(3,false,1);
 for(unsigned i=0;i<3;++i){CalCoreSubmesh::Vertex v={CalPoint4(2+i,3,4),CalVector4(0,0,1)};CalCoreSubmesh::InfluenceVector inf;inf.push_back(CalCoreSubmesh::Influence(0,.5f,false));inf.push_back(CalCoreSubmesh::Influence(1,.5f,true));sub->addVertex(v,CalColor32(0),inf);}
 sub->addFace(CalCoreSubmesh::Face(0,1,2));
 CalCoreMorphTarget::VertexOffsetArray offsets;offsets.push_back(VertexOffset(0,CalPoint4(.4f,.6f,1),CalVector4(0,0,0)));
 auto morph=std::make_shared<CalCoreMorphTarget>("known.additive",3,offsets);sub->addMorphTarget(morph);
 CalSubmesh runtime(sub);CalVector4 original[6],morphed[6];
 forward(sk,runtime,original);runtime.setMorphTargetWeight("known.additive",1);forward(sk,runtime,morphed);
 if(!near(original[0].x,1)||!near(original[0].y,0)||!near(original[0].z,4))return 2;
 if(!near(morphed[0].x-original[0].x,0)||!near(morphed[0].y-original[0].y,0)||!near(morphed[0].z-original[0].z,1))return 3;
 if(!near(morphed[2].z,original[2].z)||!near(morphed[1].z,1))return 4;
 std::puts("BASELINE_SINGULAR_SKIN_OK lost_xy=1 retained_z=1 sparse_morph=1");
 mixer.removeAnimation(animation);mixer.updateSkeleton(&sk,{},{});forward(sk,runtime,morphed);
 if(!near(morphed[0].x,2.4f)||!near(morphed[0].y,3.6f)||!near(morphed[0].z,5))return 5;
 std::puts("BASELINE_SECOND_POSE_OK restored_xyz=1 mixer=1 inverse_bind=1");
 auto mesh=std::make_shared<CalCoreMesh>();mesh->submeshes.push_back(sub);std::string bytes=CalSaver::saveCoreMeshToBuffer(mesh);if(bytes.empty())return 6;
 CalBufferSource buffer(bytes.data(),bytes.size());auto loaded=CalLoader::loadCoreMesh(buffer);if(!loaded||loaded->submeshes.size()!=1||loaded->submeshes[0]->getMorphTargets().size()!=1)return 7;
 CalSubmesh restored(loaded->submeshes[0]);restored.setMorphTargetWeight("known.additive",1);CalVector4 again[6];forward(sk,restored,again);
 if(!near(again[0].x,2.4f)||!near(again[0].y,3.6f)||!near(again[0].z,5))return 8;
 std::puts("BASELINE_CMF_MORPH_OK targets=1 final_position=1");
}
