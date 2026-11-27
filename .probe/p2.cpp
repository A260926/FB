#include <cstdio>
#include <vector>
#include <cmath>
#include <algorithm>
#include "raster_api.h"
#include "reference.h"
#include "fixtures.h"
static double md(const FT_Outline&o,int W,int H){
  std::vector<unsigned char> g((size_t)W*H,0);
  FT_Bitmap bm{};bm.width=W;bm.rows=H;bm.pitch=W;bm.buffer=g.data();bm.num_grays=256;bm.pixel_mode=FT_PIXEL_MODE_GRAY;
  FT_Raster r;ft_grays_raster.raster_new(nullptr,&r);FT_Raster_Params p{};p.source=&o;p.target=&bm;p.flags=FT_RASTER_FLAG_AA;
  ft_grays_raster.raster_render(r,&p);ft_grays_raster.raster_done(r);
  auto rr=ref::render(o,W,H,0,2e-4); double mx=0;
  for(int y=0;y<H;++y)for(int x=0;x<W;++x)mx=std::max(mx,std::fabs((double)g[(size_t)(H-1-y)*W+x]-rr.gray[(size_t)y*W+x]));
  return mx;
}
int main(){
  // Exhaustive phase search for tall conic b=24,d=320.
  int L=fx::px(1),R=fx::px(5),Hpx=160,H=Hpx*64,b=24,d=320;
  double best=1e9;int bx=0,by=0;
  for(int xf=0;xf<64;++xf)for(int yf=0;yf<64;++yf){
    fx::Shape s; s.on(L+xf,0+yf).on(R+xf,0+yf).conic(R+b+xf,H/2+d+yf).on(R+xf,H+yf).on(L+xf,H+yf).end();
    double m=md(s.outline(),8,Hpx+3);
    if(m<best){best=m;bx=xf;by=yf;}
  }
  printf("tallConic b=24 d=320 best over 1/64px phase: %.2f at (%d,%d)\n",best,bx,by);
  // Also check y-extremum: does apex actually go up/down within band?
  return 0;
}
