#include "PolyVox/RawVolume.h"
#include "PolyVox/MarchingCubesSurfaceExtractor.h"
#include "PolyVox/VolumeResampler.h"
#include <cmath>
#include <iostream>
#include <stdexcept>
using namespace PolyVox;
static void require(bool ok){if(!ok) throw std::runtime_error("production baseline mismatch");}
int main(){
 RawVolume<float> field(Region(-1,-1,-1,33,33,33));
 for(int z=-1;z<=33;++z) for(int y=-1;y<=33;++y) for(int x=-1;x<=33;++x){
   float dx=x-16.2f,dy=y-15.7f,dz=z-16.4f;
   field.setVoxel(x,y,z,100.3f-dx*dx-dy*dy-dz*dz);
 }
 auto regular=extractMarchingCubesMesh(&field,Region(0,0,0,32,32,32));
 auto decoded=decodeMesh(regular);
 require(decoded.getNoOfVertices()>0&&decoded.getNoOfIndices()>0);
 for(uint32_t i=0;i<decoded.getNoOfVertices();++i){
   auto v=decoded.getVertex(i);require(std::isfinite(v.position.getX())&&std::isfinite(v.normal.length()));
 }
 for(uint32_t i=0;i<decoded.getNoOfIndices();++i)require(decoded.getIndex(i)<decoded.getNoOfVertices());
 RawVolume<float> coarse(Region(0,0,0,16,16,16));
 VolumeResampler<RawVolume<float>,RawVolume<float>> rs(&field,Region(0,0,0,32,32,32),&coarse,coarse.getEnclosingRegion());rs.execute();
 auto reduced=extractMarchingCubesMesh(&coarse,coarse.getEnclosingRegion());
 require(reduced.getNoOfVertices()>0&&reduced.getNoOfVertices()<regular.getNoOfVertices());
 require(std::fabs(coarse.getVoxel(8,8,8)-field.getVoxel(16,16,16))<1e-4);
 std::cout<<"POLYVOX_PRODUCTION_OK fine_vertices="<<regular.getNoOfVertices()<<" fine_indices="<<regular.getNoOfIndices()<<" coarse_vertices="<<reduced.getNoOfVertices()<<" coarse_indices="<<reduced.getNoOfIndices()<<" center="<<coarse.getVoxel(8,8,8)<<"\n";
}
