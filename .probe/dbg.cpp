#include <cstdio>
#include <vector>
#include <cmath>
#include "raster_api.h"
#include "reference.h"
#include "fixtures.h"
static void dump(const FT_Outline&o,int W,int H){
  std::vector<unsigned char> g((size_t)W*H,0);
  FT_Bitmap bm{};bm.width=W;bm.rows=H;bm.pitch=W;bm.buffer=g.data();bm.num_grays=256;bm.pixel_mode=FT_PIXEL_MODE_GRAY;
  FT_Raster r;ft_grays_raster.raster_new(nullptr,&r);
  FT_Raster_Params p{};p.source=&o;p.target=&bm;p.flags=FT_RASTER_FLAG_AA;
  int e=ft_grays_raster.raster_render(r,&p);ft_grays_raster.raster_done(r);
  auto rr=ref::render(o,W,H,0);
  int nz=0,rfnz=0;double mx=0;
  for(int y=0;y<H;++y)for(int x=0;x<W;++x){ int gg=g[(size_t)(H-1-y)*W+x]; if(gg)nz++; double rv=rr.gray[(size_t)y*W+x]; if(rv>0)rfnz++; mx=std::max(mx,std::fabs(gg-rv));}
  printf("err=%d raster_nz=%d ref_nz=%d maxdiff=%.2f refAreaErr=%.3g\n",e,nz,rfnz,mx,rr.areaErr);
}
int main(){
  fx::Shape s; int L=fx::px(1),R=fx::px(5),H=100*64;
  s.on(L,0).on(R,0).conic(R+16,H/2).on(R,H).on(L,H).end();
  dump(s.outline(),8,103);
  fx::Shape a; int base=fx::px(2);
  a.on(fx::px(1),base).conic(fx::px(1)+fx::px(4)/2,base+40).on(fx::px(5),base).on(fx::px(5),base-fx::px(1)).on(fx::px(1),base-fx::px(1)).end();
  dump(a.outline(),8,9);
  return 0;
}
