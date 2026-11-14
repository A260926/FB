#include "cal3d/cal3d.h"
#include "cal3d/physiquedualquaternion.h"
#include <cmath>
#include <cstdio>
using namespace cal3d;
int main() {
  CalCoreModel core("morph-baseline");
  CalCoreSkeleton* sk=new CalCoreSkeleton;
  CalCoreBone* b=new CalCoreBone("root"); b->setParentId(-1); b->setCoreSkeleton(sk);
  b->setTranslation(CalVector(0,0,0)); b->setRotation(CalQuaternion(0,0,0,1));
  b->setTranslationBoneSpace(CalVector(0,0,0)); b->setRotationBoneSpace(CalQuaternion(0,0,0,1));
  sk->addCoreBone(b); sk->calculateState(); core.setCoreSkeleton(sk);
  CalCoreMesh* mesh=new CalCoreMesh; CalCoreSubmesh* sm=new CalCoreSubmesh;
  sm->reserve(1,0,0,0);
  CalCoreSubmesh::Vertex v; v.position=CalVector(2,1,0); v.normal=CalVector(0,1,0);
  v.collapseId=0; v.faceCollapseCount=0;
  CalCoreSubmesh::Influence inf; inf.boneId=0; inf.weight=1;v.vectorInfluence.push_back(inf);sm->setVertex(0,v);
  CalCoreSubMorphTarget* mt=new CalCoreSubMorphTarget;mt->reserve(1);
  CalCoreSubMorphTarget::BlendVertex bv;bv.position=CalVector(4,1,0);bv.normal=CalVector(1,0,0);
  mt->setBlendVertex(0,bv);sm->addCoreSubMorphTarget(mt);mesh->addCoreSubmesh(sm);core.addCoreMesh(mesh);
  CalModel model(&core);if(!model.attachMesh(0))return 2;
  model.getSkeleton()->calculateState();CalSubmesh* sub=model.getMesh(0)->getSubmesh(0);
  CalPhysique lbs(&model);CalPhysiqueDualQuat dq(&model);lbs.setNormalization(false);dq.setNormalization(false);
  float pos[3],combined[6],dpos[3];
  sub->setMorphTargetWeight(0,0);lbs.calculateVertices(sub,pos,0);
  if(std::fabs(pos[0]-2)>1e-5||std::fabs(pos[1]-1)>1e-5)return 3;
  std::puts("NORMAL_POSE_OK vertex=(2,1,0)");
  sub->setMorphTargetWeight(0,.5f);
  lbs.calculateVertices(sub,pos,0);lbs.calculateVerticesAndNormals(sub,combined,0);dq.calculateVertices(sub,dpos,0);
  std::printf("MORPH_OBSERVATION lbs=(%.6f,%.6f,%.6f) interleaved=(%.6f,%.6f,%.6f) dq=(%.6f,%.6f,%.6f) expected=(3,1,0)\n",pos[0],pos[1],pos[2],combined[0],combined[1],combined[2],dpos[0],dpos[1],dpos[2]);
  CalCoreMeshPtr loaded=CalLoader::loadCoreMesh(std::string("task_support/fixtures/sparse.xmf"));
  if(!loaded) {std::fprintf(stderr,"fixture load failed: %s\n",CalError::getLastErrorDescription().c_str());std::fprintf(stderr,"error location %s:%d %s\n",CalError::getLastErrorFile().c_str(),CalError::getLastErrorLine(),CalError::getLastErrorText().c_str());return 5;}
  CalCoreSubMorphTarget* lm=loaded->getCoreSubmesh(0)->getVectorCoreSubMorphTarget()[0];
  const CalCoreSubMorphTarget::BlendVertex* omitted=lm->getBlendVertex(0);
  const CalCoreSubMorphTarget::BlendVertex* changed=lm->getBlendVertex(1);
  std::printf("SPARSE_OBSERVATION omitted=(%.6f,%.6f,%.6f) expected=(2,1,0) changed_x=%.6f\n",omitted->position.x,omitted->position.y,omitted->position.z,changed->position.x);
  return std::isfinite(pos[0])&&std::isfinite(combined[0])&&std::isfinite(dpos[0])?0:4;
}
