#include "headless.h"
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
static void need(bool x,const char* what){if(!x){std::cerr<<what<<"\n";std::exit(1);}}
int main(){
 using namespace dragonBones;task_support::Factory f;
 std::ifstream in("task_support/rig.json");std::string json((std::istreambuf_iterator<char>(in)),{});
 auto*data=f.parseDragonBonesData(json.c_str());need(data,"parse");auto*a=f.buildArmature("rig","fixture");auto*b=f.buildArmature("rig","fixture");need(a&&b,"armatures");need(a->_armatureData==b->_armatureData,"shared asset");
 need(a->getAnimation()->play("move")&&b->getAnimation()->play("move"),"play");a->advanceTime(.25f);b->advanceTime(.25f);
 need(std::abs(a->getBone("tip")->globalTransformMatrix.tx-7)<1e-5,"animation hierarchy");need(std::abs(b->getBone("tip")->globalTransformMatrix.tx-7)<1e-5,"second instance");
 auto*slot=a->getSlot("mesh");need(slot&&slot->_deformVertices&&slot->_deformVertices->verticesData,"deform");auto*d=slot->_deformVertices;
 need(d->verticesData->weight&&d->verticesData->weight->count==6,"packed bone influences");need(d->bones.size()==2&&d->vertices.size()==12,"FFD influence layout");
 for(std::size_t i=0;i<d->vertices.size();i+=2)need(std::abs(d->vertices[i]-1)<1e-5&&std::abs(d->vertices[i+1])<1e-5,"production FFD sample");
 auto*other=b->getSlot("mesh");need(other->_deformVertices!=d&&other->_deformVertices->verticesData==d->verticesData,"instance deform ownership");
 need(static_cast<task_support::Slot*>(slot)->meshUpdates>0,"slot mesh production update");
 std::cout<<"DRAGONBONES_SETUP_BASELINE_OK shared=2 animated_tip_x="<<a->getBone("tip")->globalTransformMatrix.tx<<" ffd_x="<<d->vertices[0]<<" influences="<<d->verticesData->weight->count<<"\n";
 a->dispose();b->dispose();f.runtime.advanceTime(0);
}
