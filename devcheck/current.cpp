#include <cal3d/corebone.h>
#include <cal3d/coreanimation.h>
#include <cal3d/coreskeleton.h>
#include <cal3d/mixer.h>
#include <cal3d/skeleton.h>
#include <cassert>
#include <cmath>
#include <iostream>
CalVector tip(const ISTD::shared_ptr<CalCoreSkeleton>& core,const ISTD::shared_ptr<CalCoreAnimation>& clip) {
 CalSkeleton sk(core); CalMixer m; auto a=ISTD::make_shared<CalAnimation>(clip,1,0); a->time=.5f; m.addAnimation(a);
 m.updateSkeleton(&sk,{},{}); assert(sk.boneTransforms.size()==3); return sk.bones[2].absoluteTransform.translation;
}
int main() {
 auto sk=ISTD::make_shared<CalCoreSkeleton>(); sk->addCoreBone(ISTD::make_shared<CalCoreBone>("root"));
 auto arm=ISTD::make_shared<CalCoreBone>("arm",0); arm->relativeTransform.translation=CalVector(1,0,0); sk->addCoreBone(arm);
 auto end=ISTD::make_shared<CalCoreBone>("tip",1); end->relativeTransform.translation=CalVector(100,0,0); sk->addCoreBone(end);
 CalQuaternion q; q.setAxisAngle(CalVector(0,0,1),.01f);
 CalCoreTrack t(1,{{0,CalVector(1,0,0),CalQuaternion()},{.5f,CalVector(1,0,0),q},{1,CalVector(1,0,0),CalQuaternion()}});
 auto source=ISTD::make_shared<CalCoreAnimation>(); source->duration=1; source->tracks.push_back(t);
 auto local=t.compress(.001,1.0,nullptr); assert(local->keyframes.size()==2);
 auto compressed=ISTD::make_shared<CalCoreAnimation>(); compressed->duration=1; compressed->tracks.push_back(*local);
 auto p=tip(sk,source), r=tip(sk,compressed); double error=std::sqrt((p.x-r.x)*(p.x-r.x)+(p.y-r.y)*(p.y-r.y));
 assert(error>.9 && error<1.1); assert(t.keyframes.size()==3);
 std::cout<<"CAL3D_BASELINE_OK local_keys=2 original_keys=3 model_error_gt_0.9 bones=3\n";
}
