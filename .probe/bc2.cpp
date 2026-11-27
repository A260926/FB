#include <cstdio>
#include <vector>
#include <cmath>
#include "raster_api.h"
#include "reference.h"
#include "fixtures.h"
static void one(const char*nm,fx::Shape s,int W,int H){
  FT_Outline o=s.outline();
  auto rr=ref::render(o,W,H,0,5e-5);
  std::vector<unsigned char> g((size_t)W*H,0);
  FT_Bitmap bm{};bm.width=W;bm.rows=H;bm.pitch=W;bm.buffer=g.data();bm.num_grays=256;bm.pixel_mode=FT_PIXEL_MODE_GRAY;
  FT_Raster r;ft_grays_raster.raster_new(nullptr,&r);FT_Raster_Params p{};p.source=&o;p.target=&bm;p.flags=FT_RASTER_FLAG_AA;
  ft_grays_raster.raster_render(r,&p);ft_grays_raster.raster_done(r);
  double mx=0;int bx=0,by=0,bg=0,br=0;
  for(int y=0;y<H;++y)for(int x=0;x<W;++x){int gg=g[(size_t)(H-1-y)*W+x];double rv=rr.gray[(size_t)y*W+x];double d=std::fabs(gg-rv);if(d>mx){mx=d;bx=x;by=y;bg=gg;br=(int)rv;}}
  printf("%-18s max=%.1f at(%d,%d) g=%d ref=%d\n",nm,mx,bx,by,bg,br);
}
int main(){
  one("conicWave",fx::tallConicWave(6,24,1,0,0),9,128);
  one("cubicWave",fx::tallCubicWave(6,32,1,fx::px(9),0),16,128);
  one("arch40c",fx::conicArch(fx::px(15),40*64-20,fx::px(4),160),10,8);
  one("arch81c",fx::conicArch(fx::px(20),81*64-20,fx::px(4),160),10,8);
  one("arch10c",fx::conicArch(fx::px(25),10*64-20,fx::px(4),160),10,8);
  one("arch40k",fx::cubicArch(fx::px(30),40*64-20,fx::px(4),200),10,10);
  one("arch81k",fx::cubicArch(fx::px(35),81*64-20,fx::px(4),200),10,10);
  return 0;
}
