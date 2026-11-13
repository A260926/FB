#include <cassert>
#include <cmath>
#include <cstdio>
#include "headless.h"
int main(){
 task_support::Factory f;
 const char* json=R"({"version":"5.5","compatibleVersion":"5.5","name":"fixture","frameRate":24,"armature":[{"name":"rig","type":"Armature","frameRate":24,"bone":[{"name":"root"},{"name":"tip","parent":"root","transform":{"x":2}}],"slot":[{"name":"mesh","parent":"tip","displayIndex":0}],"skin":[{"name":"default","slot":[{"name":"mesh","display":[{"name":"triangle","type":"mesh","vertices":[0,0,2,0,0,2],"uvs":[0,0,1,0,0,1],"triangles":[0,1,2]}]}]}],"animation":[{"name":"move","duration":24,"playTimes":0,"bone":[{"name":"root","translateFrame":[{"duration":12,"tweenEasing":0,"x":0,"y":0},{"duration":12,"tweenEasing":0,"x":10,"y":0}]}]}]}]})";
 assert(f.parseDragonBonesData(json));auto arm=f.buildArmature("rig","fixture");assert(arm);
 assert(arm->getAnimation()->play("move"));arm->advanceTime(.25f);
 auto bone=arm->getBone("tip");std::fprintf(stderr,"observed tip x=%.6f\n",bone?bone->globalTransformMatrix.tx:-999.0f);assert(bone&&std::fabs(bone->globalTransformMatrix.tx-7)<1e-5);
 auto slot=arm->getSlot("mesh");assert(slot&&slot->_deformVertices&&slot->_deformVertices->verticesData);
 auto vertices=slot->_deformVertices->verticesData;auto ints=vertices->data->intArray;
 unsigned count=ints[vertices->offset+(unsigned)dragonBones::BinaryOffset::MeshVertexCount];assert(count==3);
 std::printf("DRAGONBONES_BASELINE slots=%zu vertices=%u animated_tip_x=%.6f\n",arm->getSlots().size(),count,bone->globalTransformMatrix.tx);
 arm->dispose();f.runtime.advanceTime(0);
}
