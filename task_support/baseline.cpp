#include <cstdio>
#include <numeric>
#include <set>
#include <vector>
#include "PolyVox/RawVolume.h"
#include "PolyVox/MarchingCubesSurfaceExtractor.h"
using namespace PolyVox;
int components(const Mesh<MarchingCubesVertex<float>>& mesh) {
  std::vector<unsigned> parent(mesh.getNoOfVertices());std::iota(parent.begin(),parent.end(),0);
  auto root=[&](unsigned a){while(parent[a]!=a){parent[a]=parent[parent[a]];a=parent[a];}return a;};
  for(unsigned i=0;i<mesh.getNoOfIndices();i+=3){unsigned a=root(mesh.getIndex(i));for(unsigned j=1;j<3;++j)parent[root(mesh.getIndex(i+j))]=a;}
  std::set<unsigned> groups;for(unsigned i=0;i<mesh.getNoOfIndices();++i)groups.insert(root(mesh.getIndex(i)));return groups.size();
}
int saddle(float c) {
  RawVolume<float> volume(Region(-1,-1,-1,2,2,2));
  for(int z=-1;z<=2;++z)for(int y=-1;y<=2;++y)for(int x=-1;x<=2;++x){float u=2*x-1,v=2*y-1,w=2*z-1;volume.setVoxel(x,y,z,u*v+u*w+v*w+c);}
  auto mesh=extractMarchingCubesMesh(&volume,Region(0,0,0,1,1,1));int count=components(mesh);
  std::printf("CLASSIC c=%+.2f vertices=%u triangles=%zu components=%d analytic_components=%d\n",c,mesh.getNoOfVertices(),mesh.getNoOfIndices()/3,count,c>0?1:2);
  return count;
}
int main(){
  RawVolume<float> plane(Region(-1,-1,-1,3,3,3));
  for(int z=-1;z<=3;++z)for(int y=-1;y<=3;++y)for(int x=-1;x<=3;++x)plane.setVoxel(x,y,z,float(x)-.375f);
  auto mesh=extractMarchingCubesMesh(&plane,Region(0,0,0,2,2,2));
  std::printf("NORMAL_PLANE vertices=%u triangles=%zu\n",mesh.getNoOfVertices(),mesh.getNoOfIndices()/3);
  if(!mesh.getNoOfVertices()||!mesh.getNoOfIndices())return 1;
  int a=saddle(-.25f),b=saddle(.25f);
  if(a!=2||b!=2)return 2;
  std::puts("BASELINE_CLASSIC_SIGN_ONLY_CONFIRMED");return 0;
}
