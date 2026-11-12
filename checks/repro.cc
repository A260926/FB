#include "scene.h"
#include <cassert>
#include <cstdio>
int main(int argc,char**) {
 ScenePager pager;Volume vol(&pager,1024*1024,32);
 Volume::Sampler s(&vol);s.setPosition(1,1,1);
 assert(s.getVoxel()==0);
 if(argc>1)vol.flushAll();
 else for(int k=1;k<=40;++k)vol.getVoxel(32*k,0,0);
 auto value=s.getVoxel();
 assert(value==sceneDensity(1,1,1));
 printf("SAMPLER_REBOUND_OK value=%u pages=%u\n",unsigned(value),pager.loaded);
}
