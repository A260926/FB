#include <nanovdb/tools/CreatePrimitives.h>
#include <nanovdb/HostBuffer.h>
#include <cassert>
#include <cmath>
#include <iostream>
void runNanoVDB(nanovdb::GridHandle<nanovdb::HostBuffer>&,int,int,int,nanovdb::HostBuffer&);
int main(){
 auto h=nanovdb::tools::createFogVolumeSphere<float>(6.0,nanovdb::Vec3d(0),1.0,3.0);
 auto*g=h.grid<float>(); assert(g && g->activeVoxelCount()>0);
 nanovdb::HostBuffer image(sizeof(float)); *reinterpret_cast<float*>(image.data())=-1;
 runNanoVDB(h,1,1,1,image);
 float pixel=*reinterpret_cast<float*>(image.data());
 assert(std::isfinite(pixel)&&pixel>0&&pixel<0.5);
 std::cout<<"FOG_CPU_BASELINE pixel="<<pixel<<" active="<<g->activeVoxelCount()<<"\n";
}
