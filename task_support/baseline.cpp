#include "octree.h"
#include <cmath>
#include <iostream>
#include <algorithm>
int main(){
  auto* fine=BuildOctree(glm::ivec3(-16,0,-16),32,-1.0f);
  VertexBuffer v; IndexBuffer i; GenerateMeshFromOctree(fine,v,i);
  if(v.empty()||i.empty()||i.size()%3) return 1;
  for(auto& p:v) for(int a=0;a<3;a++) if(!std::isfinite(p.xyz[a])||!std::isfinite(p.normal[a])) return 2;
  for(int n:i) if(n<0||static_cast<size_t>(n)>=v.size()) return 3;
  const auto fineCount=i.size();
  std::cout<<"FINE vertices="<<v.size()<<" triangles="<<i.size()/3<<"\n";
  DestroyOctree(fine);
  auto* coarse=BuildOctree(glm::ivec3(-16,0,-16),32,10.0f);
  GenerateMeshFromOctree(coarse,v,i);
  if(v.empty()||i.empty()||i.size()>=fineCount) return 4;
  for(int n:i) if(n<0||static_cast<size_t>(n)>=v.size()) return 5;
  std::cout<<"COARSE vertices="<<v.size()<<" triangles="<<i.size()/3<<"\n";
  DestroyOctree(coarse);
  std::cout<<"BASELINE_OK original_density_qef_contour\n";
}
