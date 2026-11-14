#include "VoxelData.hpp"
#include "VoxelOctree.hpp"
#include "Util.hpp"
#include "thread/ThreadUtils.hpp"
#include "thread/ThreadPool.hpp"
#include <cstdio>
#include <cmath>
#include <cstdlib>
#include <iostream>
static void require(bool b,const char *s){if(!b){std::cerr<<"BASELINE_FAIL "<<s<<"\n";std::exit(1);}}
int main(){
 ThreadUtils::startThreads(2);
 const char* raw=".task-build/tiny.raw";
 FILE *f=std::fopen(raw,"wb");require(f,"raw file");
 int dim[3]={8,8,8};std::fwrite(dim,sizeof(int),3,f);
 const uint32 material=compressMaterial(Vec3(1,0,0),0.6f);
 for(int z=0;z<8;++z)for(int y=0;y<8;++y)for(int x=0;x<8;++x){uint32 v=(x==2&&y==3&&z==4)?material:0;std::fwrite(&v,4,1,f);}std::fclose(f);
 VoxelData vox(raw,1024*1024);VoxelOctree tree(&vox);
 uint32 n=0;float t=-1;
 bool hit=tree.raymarch(Vec3(0,1.4375f,1.5625f),Vec3(1,0.01f,0.01f),0,n,t);
 require(hit,"ray hit");require(n==material,"packed material");require(std::fabs(t-1.25f)<1e-5f,"entry distance");
 n=0;t=-1;require(!tree.raymarch(Vec3(0,1.1f,1.1f),Vec3(1,0.01f,0.01f),0,n,t),"empty ray");
 tree.save(".task-build/tiny.oct");VoxelOctree loaded(".task-build/tiny.oct");
 n=0;t=-1;require(loaded.raymarch(Vec3(0,1.4375f,1.5625f),Vec3(1,0.01f,0.01f),0,n,t),"loaded hit");require(n==material&&std::fabs(t-1.25f)<1e-5f,"loaded identity");
 delete ThreadUtils::pool;ThreadUtils::pool=nullptr;
 std::cout<<"BASELINE_SVO_OK checks=6\n";
}
