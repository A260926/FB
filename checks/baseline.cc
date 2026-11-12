#include "scene.h"
#include "PolyVox/MarchingCubesSurfaceExtractor.h"
#include "PolyVox/RawVolume.h"
#include <cassert>
#include <cstdio>
int main() {
 ScenePager pager;Volume vol(&pager,1024*1024,32);
 Volume::Sampler s(&vol);s.setPosition(1,1,1);
 assert(s.getVoxel()==sceneDensity(1,1,1));
 s.movePositiveX();assert(s.getVoxel()==sceneDensity(2,1,1));
 assert(s.peekVoxel1px1py1pz()==sceneDensity(3,2,2));
 PolyVox::Region r(-7,-7,-7,7,7,7);
 PolyVox::RawVolume<uint8_t> raw(r);
 for(int z=-7;z<=7;++z)for(int y=-7;y<=7;++y)for(int x=-7;x<=7;++x)
  raw.setVoxel(x,y,z,sceneDensity(x,y,z));
 auto a=PolyVox::extractMarchingCubesMesh(&vol,r);
 auto b=PolyVox::extractMarchingCubesMesh(&raw,r);
 assert(a.getNoOfVertices()>0 && a.getNoOfIndices()>0);
 assert(a.getNoOfVertices()==b.getNoOfVertices() && a.getNoOfIndices()==b.getNoOfIndices());
 printf("POLYVOX_BASELINE_OK vertices=%u indices=%zu\n",a.getNoOfVertices(),a.getNoOfIndices());
}
