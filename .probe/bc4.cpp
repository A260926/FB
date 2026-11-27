#include <cstdio>
#include <vector>
#include <cmath>
#include "raster_api.h"
#include "reference.h"
#include "fixtures.h"
static void one(const char*nm,fx::Shape s,int W,int H,int base){
  FT_Outline o=s.outline();
  auto rr=ref::render(o,W,H,0,5e-5);
  std::vector<unsigned char> g((size_t)W*H,0);
  FT_Bitmap bm{};bm.width=W;bm.rows=H;bm.pitch=W;bm.buffer=g.data();bm.num_grays=256;bm.pixel_mode=FT_PIXEL_MODE_GRAY;
  FT_Raster r;ft_grays_raster.raster_new(nullptr,&r);FT_Raster_Params p{};p.source=&o;p.target=&bm;p.flags=FT_RASTER_FLAG_AA;
  ft_grays_raster.raster_render(r,&p);ft_grays_raster.raster_done(r);
  double mx=0;int bx=0,by=0;
  for(int y=0;y<H;++y)for(int x=0;x<W;++x){double d=std::fabs((double)g[(size_t)(H-1-y)*W+x]-rr.gray[(size_t)y*W+x]);if(d>mx){mx=d;bx=x;by=y;}}
  printf("%-22s base=%d max=%.1f at(%d,%d)\n",nm,base,mx,bx,by);
}
int main(){
  int W=16,H=100;
  one("arch40 cubic200",fx::cubicArch(fx::px(2),40*64-20,fx::px(4),200),W,H,40);
  one("arch40 cubic160",fx::cubicArch(fx::px(2),40*64-20,fx::px(4),160),W,H,40);
  one("arch81 cubic160",fx::cubicArch(fx::px(2),81*64-20,fx::px(4),160),W,H,81);
  one("arch10 cubic160",fx::cubicArch(fx::px(2),10*64-20,fx::px(4),160),W,H,10);
  one("arch40 conic160",fx::conicArch(fx::px(2),40*64-20,fx::px(4),160),W,H,40);
  return 0;
}
