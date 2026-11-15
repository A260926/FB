#include "storage.h"
#include "raytracing.h"
#include "extraction.h"
#include <cmath>
#include <iostream>
int main(){
 using namespace Cubiquity;
 Volume v;
 for(int z=1;z<=3;z++)for(int y=1;y<=3;y++)for(int x=1;x<=3;x++) v.setVoxel(x,y,z,7);
 for(int z=-4;z<=-2;z++)for(int y=-4;y<=-2;y++)for(int x=-4;x<=-2;x++) v.setVoxel(x,y,z,9);
 auto check=[&](){
  auto sub=findSubDAGs(Internals::getNodes(v),v.rootNodeIndex());
  auto hit=intersectVolume(v,sub,-10.0f,2.0f,2.0f,1.0f,0.013f,0.007f,true);
  auto ref=traceRayRef(v,-10.0f,2.0f,2.0f,1.0f,0.013f,0.007f);
  if(!hit.hit||!ref.hit||hit.material!=7||ref.material!=7||std::abs(hit.distance-ref.distance)>1e-4) return false;
  std::cout<<"RAY material="<<hit.material<<" distance="<<hit.distance<<"\n";
  return true;
 };
 if(!check())return 1;
 auto before=v.countNodes();v.bake();
 if(v.voxel(2,2,2)!=7||v.voxel(-3,-3,-3)!=9||v.voxel(0,0,0)!=0||!check())return 2;
 Glyph glyphs[128];auto n=extractGlyphs(v,false,glyphs,128);if(!n||n>128)return 3;
 std::cout<<"DAG before="<<before<<" after="<<v.countNodes()<<" glyphs="<<n<<"\n";
 std::cout<<"BASELINE_OK storage_ray_glyph\n";
}
