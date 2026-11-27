// Joint search: for each curved fixture family, scan curvature/phase params at
// 1/8 px and print the concrete integer geometry with the smallest
// raster-vs-reference deviation.
#include <cstdio>
#include <vector>
#include <cmath>
#include <algorithm>
#include "raster_api.h"
#include "reference.h"
#include "fixtures.h"

static double md( const FT_Outline& o, int W,int H,int fl=0){
  std::vector<unsigned char> g((size_t)W*H,0);
  FT_Bitmap bm{};bm.width=W;bm.rows=H;bm.pitch=W;bm.buffer=g.data();bm.num_grays=256;bm.pixel_mode=FT_PIXEL_MODE_GRAY;
  FT_Raster r;ft_grays_raster.raster_new(nullptr,&r);FT_Raster_Params p{};p.source=&o;p.target=&bm;p.flags=FT_RASTER_FLAG_AA;
  ft_grays_raster.raster_render(r,&p);ft_grays_raster.raster_done(r);
  auto rr=ref::render(o,W,H,fl,1.5e-4);double mx=0;
  for(int y=0;y<H;++y)for(int x=0;x<W;++x)mx=std::max(mx,std::fabs((double)g[(size_t)(H-1-y)*W+x]-rr.gray[(size_t)y*W+x]));
  return mx;
}

struct Best{ double err; int a,b,c,d; };

int main(){
  // A: tall conic: L..R verticals, control (R+b, H*frac + yoff)
  {
    Best best{1e9,0,0,0,0};
    int Hpx=160,H=Hpx*64,L=fx::px(1),R=fx::px(5);
    for(int b=8;b<=48;b+=4) for(int fr=0;fr<16;++fr) for(int xf=0;xf<32;++xf) for(int yf=0;yf<32;++yf){
      int cy=H*fr/16+yf*2;
      fx::Shape s; s.on(L+xf,yf).on(R+xf,yf).conic(R+b+xf,cy).on(R+xf,H+yf).on(L+xf,H+yf).end();
      double e=md(s.outline(),8,Hpx+3);
      if(e<best.err){best={e,b,fr,xf,yf};}
      if(e<=1.0 && b<=40) { /* keep searching for smallest */ }
    }
    printf("tallConic best err=%.2f b=%d fr=%d xoff=%d yoff=%d\n",best.err,1,best.b,best.c,best.d);
  }
  return 0;
}
