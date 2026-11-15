#include "src/ofbx.h"
#include <cmath>
#include <fstream>
#include <iostream>
#include <iterator>
#include <vector>
static void need(bool x,const char* what){if(!x){std::cerr<<what<<"\n";std::exit(1);}}
static ofbx::DVec3 point(const ofbx::DMatrix&m,ofbx::DVec3 p){return {m.m[0]*p.x+m.m[4]*p.y+m.m[8]*p.z+m.m[12],m.m[1]*p.x+m.m[5]*p.y+m.m[9]*p.z+m.m[13],m.m[2]*p.x+m.m[6]*p.y+m.m[10]*p.z+m.m[14]};}
int main(){
 std::ifstream f("task_support/rig.fbx",std::ios::binary);std::vector<unsigned char>b((std::istreambuf_iterator<char>(f)),{});
 auto*s=ofbx::load(b.data(),int(b.size()),0);need(s!=nullptr,ofbx::getError());need(s->getMeshCount()==1,"mesh count");
 const ofbx::Object*bone=nullptr;for(int i=0;i<s->getAllObjectCount();++i)if(s->getAllObjects()[i]->id==2)bone=s->getAllObjects()[i];need(bone,"bone");
 auto p=point(bone->getGlobalTransform(),{1,0,0});need(std::abs(p.x-3)<1e-8&&std::abs(p.y-4)<1e-8,"static hierarchy");
 auto*st=s->getAnimationStack(0);need(st&&st->getLayer(0),"animation layer");auto*c=st->getLayer(0)->getCurveNode(*bone,"Lcl Rotation");need(c,"curve node");auto r=c->getNodeLocalTransform(.5);need(std::abs(r.z-45)<1e-6,"curve sample");
 auto l=bone->evalLocal(bone->getLocalTranslation(),r,bone->getLocalScaling());auto lp=point(l,{1,0,0});const double k=std::sqrt(.5);need(std::abs(lp.x-(1.5+.5*k))<1e-7&&std::abs(lp.y-.5*k)<1e-7,"pivot production path");
 auto*m=s->getMesh(0);need(m->getSkin()&&m->getSkin()->getClusterCount()==1,"skin");auto*cl=m->getSkin()->getCluster(0);need(cl->getLink()==bone&&cl->getIndicesCount()==3,"cluster links");
 auto w=bone->getParent()->getGlobalTransform();ofbx::DMatrix world{};for(int col=0;col<4;++col)for(int row=0;row<4;++row)for(int j=0;j<4;++j)world.m[col*4+row]+=w.m[j*4+row]*l.m[col*4+j];auto sk=point(world,{1,0,0});need(std::abs(sk.x-(3-.5*k))<1e-7&&std::abs(sk.y-(3+k))<1e-7,"animated skin point");
 std::cout<<"OPENFBX_BASELINE_OK meshes=1 curve_z="<<r.z<<" skin_point="<<sk.x<<","<<sk.y<<"\n";s->destroy();
}
