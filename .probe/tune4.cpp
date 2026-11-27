// Measure arch fixtures (vertical curvature forces subdivision) and wavy tall
// boundaries, both pools, over fine phase.  Goal <=3.5 gray.
#include <cstdio>
#include <vector>
#include <cmath>
#include <algorithm>
#include "raster_api.h"
#include "reference.h"
#include "fixtures.h"

static const FT_Raster_Funcs* ap[2]={&ft_grays_raster,&ft_grays_raster_small};
static double md( const FT_Outline& o, int W,int H,int fl=0){
  double worst=0;
  for(int a=0;a<2;++a){
    std::vector<unsigned char> g((size_t)W*H,0);
    FT_Bitmap bm{};bm.width=W;bm.rows=H;bm.pitch=W;bm.buffer=g.data();bm.num_grays=256;bm.pixel_mode=FT_PIXEL_MODE_GRAY;
    FT_Raster r;ap[a]->raster_new(nullptr,&r);FT_Raster_Params p{};p.source=&o;p.target=&bm;p.flags=FT_RASTER_FLAG_AA;
    ap[a]->raster_render(r,&p);ap[a]->raster_done(r);
    auto rr=ref::render(o,W,H,fl,1.5e-4);double mx=0;
    for(int y=0;y<H;++y)for(int x=0;x<W;++x)mx=std::max(mx,std::fabs((double)g[(size_t)(H-1-y)*W+x]-rr.gray[(size_t)y*W+x]));
    worst=std::max(worst,mx);
  }
  return worst;
}
// arch: base at yb, width wpx, apex rise hsub; filled below base via 1px band.
static fx::Shape archConic(int x0,int yb,int wpx,int hsub,int xo,int yo){
  fx::Shape s; s.on(x0+xo,yb+yo).conic(x0+wpx/2+xo,yb+hsub+yo).on(x0+wpx+xo,yb+yo)
    .on(x0+wpx+xo,yb-fx::px(1)+yo).on(x0+xo,yb-fx::px(1)+yo).end(); return s;
}
static fx::Shape archCubic(int x0,int yb,int wpx,int hsub,int xo,int yo){
  fx::Shape s; s.on(x0+xo,yb+yo).cubic(x0+wpx/3+xo,yb+hsub+yo).cubic(x0+2*wpx/3+xo,yb+hsub+yo)
    .on(x0+wpx+xo,yb+yo).on(x0+wpx+xo,yb-fx::px(1)+yo).on(x0+xo,yb-fx::px(1)+yo).end(); return s;
}
// wavy tall right boundary: N humps alternating x bulge, spanning Hpx rows.
static fx::Shape waveCubic(int Hpx,int N,int b,int w,int xo,int yo){
  fx::Shape s; int L=fx::px(1),R=fx::px(5+w),H=Hpx*64;
  s.on(L+xo,yo).on(R+xo,yo);
  for(int i=0;i<N;++i){
    int y0=H*i/N, y1=H*(i+1)/N;
    int sg=(i%2)?-1:1; int d=2*64; // 2px vertical control offset forces splits
    s.cubic(R+sg*b+xo,y0+d+yo).cubic(R+sg*b+xo,y1-d+yo).on(R+xo,y1+yo);
  }
  s.on(L+xo,H+yo).end(); return s;
}
static fx::Shape waveConic(int Hpx,int N,int b,int w,int xo,int yo){
  fx::Shape s; int L=fx::px(1),R=fx::px(5+w),H=Hpx*64;
  s.on(L+xo,yo).on(R+xo,yo);
  for(int i=0;i<N;++i){
    int y0=H*i/N, y1=H*(i+1)/N; int sg=(i%2)?-1:1;
    // conic with control at mid, x bulge sg*b, y offset d to force splits
    s.conic(R+sg*b+xo,H*i/N+H/(2*N)+yo*0+ (H/(2*N))+0).on(R+xo,y1+yo);
  }
  s.on(L+xo,H+yo).end(); return s;
}

int main(){
  printf("ARCH conic (w px, apex h units):\n");
  for(int wpx : {4,5,6}) for(int hs : {160,200,240,288}) {
    double best=1e9;int bx=0,by=0;
    for(int xo=0;xo<64;xo+=4)for(int yo=0;yo<64;yo+=4){
      double e=md(archConic(fx::px(1),fx::px(2),fx::px(wpx),hs,xo,yo).outline(),8,(hs/64)+6);
      if(e<best){best=e;bx=xo;by=yo;}
    }
    printf("  w=%d h=%3d(%.2fpx) best=%.2f @(%d,%d)%s\n",wpx,hs,hs/64.0,best,bx,by,best<=3.5?"  FIT":"");
  }
  printf("ARCH cubic:\n");
  for(int wpx : {4,5,6}) for(int hs : {160,200,240,288}) {
    double best=1e9;int bx=0,by=0;
    for(int xo=0;xo<64;xo+=4)for(int yo=0;yo<64;yo+=4){
      double e=md(archCubic(fx::px(1),fx::px(2),fx::px(wpx),hs,xo,yo).outline(),8,(hs/64)+6);
      if(e<best){best=e;bx=xo;by=yo;}
    }
    printf("  w=%d h=%3d(%.2fpx) best=%.2f @(%d,%d)%s\n",wpx,hs,hs/64.0,best,bx,by,best<=3.5?"  FIT":"");
  }
  printf("WAVE cubic H=120, N humps, b bulge, w width:\n");
  for(int N : {4,6}) for(int b : {16,24,32}) for(int w : {4,6}) {
    double best=1e9;int bx=0,by=0;
    for(int xo=0;xo<64;xo+=8)for(int yo=0;yo<64;yo+=8){
      double e=md(waveCubic(120,N,b,w,xo,yo).outline(),8+w,123);
      if(e<best){best=e;bx=xo;by=yo;}
    }
    printf("  N=%d b=%d w=%d best=%.2f @(%d,%d)%s\n",N,b,w,best,bx,by,best<=3.5?"  FIT":"");
  }
  return 0;
}
