#include "VoxelData.hpp"
#include "VoxelOctree.hpp"
#include "thread/ThreadUtils.hpp"
#include "thread/ThreadPool.hpp"
#include <fstream>
#include <vector>
#include <cmath>
#include <iostream>
#include <stdexcept>
static void require(bool ok){if(!ok)throw std::runtime_error("CPU production baseline mismatch");}
int main(){
 ThreadUtils::startThreads(2);
 const uint32 n=8,payload=0x3456789au;
 const char* raw=".task-build/scene.raw";const char* oct=".task-build/scene.oct";
 std::vector<uint32> data(n*n*n,0);
 for(uint32 z=2;z<6;++z)for(uint32 y=2;y<6;++y)for(uint32 x=2;x<6;++x)data[x+n*(y+n*z)]=payload;
 {std::ofstream f(raw,std::ios::binary);for(int k=0;k<3;++k)f.write(reinterpret_cast<const char*>(&n),4);f.write(reinterpret_cast<const char*>(data.data()),data.size()*4);require(bool(f));}
 float first=0,second=0;uint32 value=0;
 {
  VoxelData input(raw,1024*1024);VoxelOctree tree(&input);
  require(tree.raymarch(Vec3(0,1.43f,1.57f),Vec3(1,0,0),0,value,first));require(value==payload&&std::fabs(first-1.25f)<1e-4f);
  require(!tree.raymarch(Vec3(0,1.05f,1.05f),Vec3(1,0,0),0,value,second));
  tree.save(oct);
 }
 {
  VoxelOctree reloaded(oct);
  require(reloaded.raymarch(Vec3(3,1.43f,1.57f),Vec3(-1,0,0),0,value,second));require(value==payload&&std::fabs(second-1.25f)<1e-4f);
 }
 delete ThreadUtils::pool;ThreadUtils::pool=nullptr;
 std::cout<<"SVO_PRODUCTION_OK payload="<<value<<" forward_t="<<first<<" reload_reverse_t="<<second<<" miss=1\n";
}
