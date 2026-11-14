#include <cstdio>
#include <cmath>
#include "PolyVox/RawVolume.h"
#include "PolyVox/TrilinearSurfaceExtractor.h"
int task_production_smoke() {
  PolyVox::RawVolume<float> volume(PolyVox::Region(-1,-1,-1,3,3,3));
  for(int z=-1;z<=3;++z)for(int y=-1;y<=3;++y)for(int x=-1;x<=3;++x)volume.setVoxel(x,y,z,float(x)-.375f);
  auto mesh=PolyVox::extractTrilinearMesh(&volume,PolyVox::Region(0,0,0,2,2,2));
  if(!mesh.getNoOfVertices()||!mesh.getNoOfIndices())return 1;
  for(unsigned i=0;i<mesh.getNoOfVertices();++i)if(std::abs(mesh.getVertex(i).position.getX()-.375f)>1e-5f)return 2;
  std::printf("PRODUCTION_SMOKE vertices=%u triangles=%zu\n",mesh.getNoOfVertices(),mesh.getNoOfIndices()/3);return 0;
}
