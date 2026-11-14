#include <cal3d/corebone.h>
#include <cal3d/coreskeleton.h>
#include <cal3d/coreanimation.h>
#include <cal3d/mixer.h>
#include <cal3d/skeleton.h>
#include <cal3d/physique.h>
#include <cassert>
#include <cmath>
#include <iostream>
static bool near(float a,float b){return std::fabs(a-b)<1e-5;}
int main(){
 auto core=ISTD::make_shared<CalCoreSkeleton>();
 core->addCoreBone(ISTD::make_shared<CalCoreBone>("root"));
 auto child=ISTD::make_shared<CalCoreBone>("tip",0);
 child->relativeTransform.translation=CalVector(2,0,0);
 child->inverseBindPoseTransform.translation=CalVector(-1,0,0);
 core->addCoreBone(child);
 CalSkeleton sk(core); CalMixer mixer;
 CalQuaternion q; q.setAxisAngle(CalVector(0,0,1),1.57079632679f);
 auto clip=ISTD::make_shared<CalCoreAnimation>(); clip->duration=1;
 clip->tracks.push_back(CalCoreTrack(0,{{0,CalVector(3,0,0),q}}));
 auto animation=ISTD::make_shared<CalAnimation>(clip,1,0); mixer.addAnimation(animation);
 mixer.updateSkeleton(&sk,{},{});
 assert(near(sk.bones[1].absoluteTransform.translation.x,3));
 assert(near(sk.bones[1].absoluteTransform.translation.y,2));
 CalCoreSubmesh::Vertex v[]={{CalPoint4(1,0,0),CalVector4(1,0,0)}};
 CalCoreSubmesh::Influence inf[]={ {0,.25f,false},{1,.75f,true} };
 CalVector4 out[2];CalPhysique::calculateVerticesAndNormals_x87(&sk.boneTransforms[0],1,v,inf,out);
 assert(near(out[0].x,3) && near(out[0].y,1.75f));
 assert(near(out[1].x,0) && near(out[1].y,1));
 mixer.removeAnimation(animation);mixer.updateSkeleton(&sk,{},{});
 assert(near(sk.bones[1].absoluteTransform.translation.x,2));
 assert(near(core->coreBones[1]->relativeTransform.translation.x,2));
 std::cout<<"CAL3D_PIPELINE_OK mixed_pose=1 inverse_bind=1 weighted_skin=1 reset=1\n";
}
