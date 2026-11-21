#include "storage.h"
#include "raytracing.h"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <vector>
using namespace Cubiquity;
struct Cell { int x,y,z; MaterialId material; };
bool slab(const Cell& c, const vec3f& o, const vec3f& d, double& t) {
    double lo=0,hi=std::numeric_limits<double>::infinity();
    int v[3]={c.x,c.y,c.z};
    for(int a=0;a<3;++a) {
        double t0=(double(v[a])-.5-o[a])/d[a],t1=(double(v[a])+.5-o[a])/d[a];
        if(t0>t1) std::swap(t0,t1);
        lo=std::max(lo,t0);hi=std::min(hi,t1);
    }
    t=lo;return lo<hi;
}
int main() {
    Volume volume; std::vector<Cell> cells;
    for(int z=0;z<3;++z) for(int y=0;y<3;++y) for(int x=2;x<5;++x) cells.push_back({x,y,z,7});
    for(int z=-2;z<1;++z) for(int y=-3;y<0;++y) for(int x=-4;x<-1;++x) cells.push_back({x,y,z,9});
    for(auto c:cells) volume.setVoxel(c.x,c.y,c.z,c.material);
    volume.bake();
    auto sub=findSubDAGs(Internals::getNodes(volume),volume.rootNodeIndex());
    unsigned hits=0,misses=0;
    for(int i=0;i<20;++i) {
        vec3f o=i<8?vec3f(-12,-.2f+.12f*i,.1f):i<16?vec3f(-12,-2.3f+.08f*(i-8),-1.2f):vec3f(-12,20,20);
        vec3f d=normalize(vec3f(1,.013f,.017f));
        double nearest=std::numeric_limits<double>::infinity();MaterialId material=0;
        for(auto c:cells) {double t;if(slab(c,o,d,t)&&t<nearest){nearest=t;material=c.material;}}
        auto fast=intersectVolume(volume,sub,Ray3f(o,d),true);
        auto slow=traceRayRef(volume,Ray3f(o,d));
        if(fast.hit!=(material!=0) || slow.hit!=fast.hit) return 1;
        if(fast.hit) {
            if(fast.material!=material || slow.material!=material || std::abs(fast.distance-nearest)>1e-4 || std::abs(slow.distance-nearest)>1e-4) return 2;
            ++hits;
        } else ++misses;
    }
    if(hits!=16 || misses!=4) return 3;
    std::cout<<"CUBIQUITY_BASELINE rays=20 hits="<<hits<<" misses="<<misses<<" oracle=voxel_boxes materials=7,9\n";
}
