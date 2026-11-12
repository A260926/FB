#include "frame.hpp"
#include <cassert>
#include <iostream>
int main(){
 using namespace render;
 Camera current({0,0,0},{0,0,1},{0,1,0},1.2);
 Camera previous({.1,0,0},{.1,0,1},{0,1,0},1.2);
 Triangle tri{{-1,-1,3},{1,-1,3},{0,1,3},{},{},{3,1,.5},7};
 Frame f=draw({tri},current,previous,48,32,{.25,-.25},2,10);
 size_t i=16*48+24;
 assert(f.object[i]==7&&f.depth[i]>2.99&&f.depth[i]<3.01);
 assert(f.previous_depth[i]>2.99&&std::abs(f.motion[i].x)>.1);
 assert(finite(f.hdr.color[i]));
 V3 world=current.unproject({24.5,16.5},f.depth[i],48,32,f.jitter);
 auto p=current.project(world,48,32,f.jitter);assert(length(V3{p.pixel.x-24.5,p.pixel.y-16.5,0})<1e-10);
 Image small(2,2);small.at(0,0)={0,0,0};small.at(1,0)={2,0,0};small.at(0,1)={0,2,0};small.at(1,1)={2,2,0};
 V3 mid=small.bilinear({1,1});assert(mid.x==1&&mid.y==1);
 std::cout<<"RASTER_GBUFFER_BASELINE_OK object=7 depth=3 motion=nonzero\n";
}
