#pragma once
#include "PolyVox/PagedVolume.h"
#include <algorithm>
#include <cstdint>
using Volume = PolyVox::PagedVolume<uint8_t>;
inline uint8_t sceneDensity(int x,int y,int z) { return x*x+y*y+z*z<25 ? 0 : 255; }
struct ScenePager : Volume::Pager {
  unsigned loaded=0;
  void pageIn(const PolyVox::Region& r, Volume::Chunk* chunk) override {
    ++loaded;
    for(unsigned z=0;z<32;++z)for(unsigned y=0;y<32;++y)for(unsigned x=0;x<32;++x)
      chunk->setVoxel(x,y,z,sceneDensity(r.getLowerX()+x,r.getLowerY()+y,r.getLowerZ()+z));
  }
  void pageOut(const PolyVox::Region&, Volume::Chunk*) override {}
};
