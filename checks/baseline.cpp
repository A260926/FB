#include "cal3d/cal3d.h"
#include "cal3d/coretrack.h"
#include "cal3d/corekeyframe.h"

#include "cal3d/animcallback.h"
#include <cmath>
#include <cstdio>
using namespace cal3d;
static int require(bool b,const char* msg){if(!b){std::fprintf(stderr,"BASELINE_FAIL %s\n",msg);return 1;}return 0;}
struct Observe:CalAnimationCallback{
 int count; float seen; CalSubmesh* sub;
 Observe(CalSubmesh*s):count(0),seen(0),sub(s){}
 void AnimationUpdate(float,CalModel*m,void*){++count;seen=m->getPhysique()->calculateVertex(sub,0).x;}
 void AnimationComplete(CalModel*,void*){}
};
int main(){
 CalCoreModel core("baseline");
 CalCoreSkeleton* sk=new CalCoreSkeleton;
 CalCoreBone* r=new CalCoreBone("root");r->setParentId(-1);r->setTranslation(CalVector(2,0,0));r->setRotation(CalQuaternion(0,0,0,1));r->setTranslationBoneSpace(CalVector(-2,0,0));r->setRotationBoneSpace(CalQuaternion(0,0,0,1));sk->addCoreBone(r);sk->calculateState();core.setCoreSkeleton(sk);
 CalCoreAnimation* anim=new CalCoreAnimation;anim->setDuration(1);
 CalCoreTrack* tr=new CalCoreTrack;tr->setCoreBoneId(0);tr->setTranslationRequired(true);
 for(int i=0;i<2;++i){CalCoreKeyframe*k=new CalCoreKeyframe;k->setTime(float(i));k->setTranslation(CalVector(2+4*i,0,0));k->setRotation(CalQuaternion(0,0,0,1));tr->addCoreKeyframe(k);}anim->addCoreTrack(tr);core.addCoreAnimation(anim);
 CalCoreMesh* mesh=new CalCoreMesh;CalCoreSubmesh* cs=new CalCoreSubmesh;cs->reserve(1,0,0,0);CalCoreSubmesh::Vertex v;v.position=CalVector(3,0,0);v.normal=CalVector(0,1,0);v.collapseId=0;v.faceCollapseCount=0;CalCoreSubmesh::Influence inf;inf.boneId=0;inf.weight=1;v.vectorInfluence.push_back(inf);cs->setVertex(0,v);mesh->addCoreSubmesh(cs);core.addCoreMesh(mesh);
 CalModel model(&core);model.attachMesh(0);CalSubmesh* sub=model.getVectorMesh()[0]->getSubmesh(0);Observe obs(sub);anim->registerCallback(&obs,0);
 model.getMixer()->executeAction(0,0,0,1,true);model.update(0.25f);
 CalVector p=model.getPhysique()->calculateVertex(sub,0);float root=model.getSkeleton()->getBone(0)->getTranslationAbsolute().x;
 if(require(std::fabs(root-3)<1e-5&&std::fabs(p.x-4)<1e-5,"animated inverse-bind skin"))return 1;
 std::printf("CAL3D_PRODUCTION_OK root=%.3f skinned=%.3f callbacks=%d callback_seen=%.3f\n",root,p.x,obs.count,obs.seen);
 anim->removeCallback(&obs);return 0;
}
