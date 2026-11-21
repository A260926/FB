#include <cmath>
#include <iostream>
#include <map>
#include <tuple>
#include <vector>
#include "PolyVox/RawVolume.h"
#include "PolyVox/PagedVolume.h"
#include "PolyVox/CubicSurfaceExtractor.h"
using namespace PolyVox;
struct Pager final : PagedVolume<uint8_t>::Pager {
    using Chunk = PagedVolume<uint8_t>::Chunk;
    void pageIn(const Region& r, Chunk* c) override {
        for (int z=0;z<8;++z) for(int y=0;y<8;++y) for(int x=0;x<8;++x) c->setVoxel(x,y,z,0);
        (void)r;
    }
    void pageOut(const Region&, Chunk*) override {}
};
template<class V> void fill(V& v) {
    for(int z=-8;z<8;++z) for(int y=-8;y<8;++y) for(int x=-8;x<8;++x)
        v.setVoxel(x,y,z,(x>=-2&&x<2&&y>=-2&&y<2&&z>=-2&&z<2)?7:0);
}
template<class M> double signedVolume(const M& m) {
    double s=0;
    for(size_t i=0;i<m.getNoOfIndices();i+=3) {
        auto a=decodeVertex(m.getVertex(m.getIndex(i))).position;
        auto b=decodeVertex(m.getVertex(m.getIndex(i+1))).position;
        auto c=decodeVertex(m.getVertex(m.getIndex(i+2))).position;
        s += (a.getX()*(b.getY()*c.getZ()-b.getZ()*c.getY())+
              a.getY()*(b.getZ()*c.getX()-b.getX()*c.getZ())+
              a.getZ()*(b.getX()*c.getY()-b.getY()*c.getX()))/6.0;
    }
    return s;
}
int main() {
    RawVolume<uint8_t> raw(Region(-8,-8,-8,7,7,7));
    Pager pager;
    auto paged=std::make_unique<PagedVolume<uint8_t>>(&pager,1048576,8);
    fill(raw); fill(*paged);
    auto a=extractCubicMesh(&raw,Region(-5,-5,-5,4,4,4));
    auto b=extractCubicMesh(paged.get(),Region(-5,-5,-5,4,4,4));
    if(a.getNoOfIndices()!=36 || b.getNoOfIndices()!=36 ||
       a.getNoOfVertices()!=b.getNoOfVertices() ||
       std::abs(signedVolume(a)-64)>1e-8 || std::abs(signedVolume(b)-64)>1e-8) return 1;
    for(size_t i=0;i<a.getNoOfVertices();++i) {
        const auto av=a.getVertex(i),bv=b.getVertex(i);
        if(av.encodedPosition!=bv.encodedPosition || av.data!=7 || bv.data!=7) return 2;
    }
    for(size_t i=0;i<a.getNoOfIndices();++i) if(a.getIndex(i)!=b.getIndex(i)) return 3;
    std::cout<<"POLYVOX_BASELINE raw_indices="<<a.getNoOfIndices()<<" paged_indices="<<b.getNoOfIndices()<<" volume="<<signedVolume(a)<<" material=7\n";
}
