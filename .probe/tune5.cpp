// Phase-search specific fixtures to bring each <=3.5 gray on both pools.
#include <cstdio>
#include <vector>
#include <cmath>
#include <algorithm>
#include "raster_api.h"
#include "reference.h"
#include "fixtures.h"

static double md(const FT_Outline&o,int W,int H,int fl=0){
  double worst=0;
  for(int a=0;a<2;++a){
    const FT_Raster_Funcs* api = a? &ft_grays_raster_small : &ft_grays_raster;
    std::vector<unsigned char> g((size_t)W*H,0);
    FT_Bitmap bm{};bm.width=W;bm.rows=H;bm.pitch=W;bm.buffer=g.data();bm.num_grays=256;bm.pixel_mode=FT_PIXEL_MODE_GRAY;
    FT_Raster r;api->raster_new(nullptr,&r);FT_Raster_Params p{};p.source=&o;p.target=&bm;p.flags=FT_RASTER_FLAG_AA;
    api->raster_render(r,&p);api->raster_done(r);
    auto rr=ref::render(o,W,H,fl,1e-4);double mx=0;
    for(int y=0;y<H;++y)for(int x=0;x<W;++x)mx=std::max(mx,std::fabs((double)g[(size_t)(H-1-y)*W+x]-rr.gray[(size_t)y*W+x]));
    worst=std::max(worst,mx);
  }
  return worst;
}

int main(){
  // impliedOffEnd: perturb offsets
  {
    double best=1e9;int bx=0,by=0;
    fx::Shape base=fx::impliedConicStartOffEnd();
    for(int dx=-16;dx<=16;++dx)for(int dy=-16;dy<=16;++dy){
      double e=md(base.translated(dx,dy).outline(),10,8);
      if(e<best){best=e;bx=dx;by=dy;}
    }
    printf("impliedOffEnd best=%.2f @(%d,%d)\n",best,bx,by);
  }
  // layoutMixed: perturb
  {
    double best=1e9;int bx=0,by=0;
    fx::Shape base=fx::layoutMixed();
    for(int dx=-16;dx<=16;++dx)for(int dy=-16;dy<=16;++dy){
      double e=md(base.translated(dx,dy).outline(),20,14);
      if(e<best){best=e;bx=dx;by=dy;}
    }
    printf("layoutMixed best=%.2f @(%d,%d)\n",best,bx,by);
  }
  // bandsCurves: separate each arch, tune arc phases, and waves phases
  {
    // tall waves
    for(int b : {20,24,28,32}) {
      double best=1e9;int bx=0,by=0;
      for(int xo=0;xo<64;xo+=4)for(int yo=0;yo<64;yo+=4){
        double e=md(fx::tallConicWave(6,b,1,xo,yo).outline(),8,123);
        if(e<best){best=e;bx=xo;by=yo;}
      }
      printf("tallConicWave b=%d best=%.2f @(%d,%d)\n",b,best,bx,by);
    }
    for(int b : {24,32,40}) {
      double best=1e9;int bx=0,by=0;
      for(int xo=0;xo<64;xo+=4)for(int yo=0;yo<64;yo+=4){
        double e=md(fx::tallCubicWave(6,b,1,xo,yo).outline(),14,123);
        if(e<best){best=e;bx=xo;by=yo;}
      }
      printf("tallCubicWave b=%d best=%.2f @(%d,%d)\n",b,best,bx,by);
    }
    // arches straddling band lines: try different wpx/hsub/phase
    for(int wpx : {4,5,6}) for(int hs : {120,160,200}) {
      double best=1e9;int bx=0,by=0;
      for(int xo=0;xo<64;xo+=4)for(int yo=0;yo<32;yo+=4){
        fx::Shape s=fx::conicArch(fx::px(2),40*64-20,wpx*64,hs,xo).translated(0,yo);
        double e=md(s.outline(),12,(hs+63)/64+6);
        if(e<best){best=e;bx=xo;by=yo;}
      }
      printf("conicArch w=%d hs=%d best=%.2f @(%d,%d)\n",wpx,hs,best,bx,by);
    }
    for(int wpx : {4,5,6}) for(int hs : {160,200,240}) {
      double best=1e9;int bx=0,by=0;
      for(int xo=0;xo<64;xo+=4)for(int yo=0;yo<32;yo+=4){
        fx::Shape s=fx::cubicArch(fx::px(2),40*64-20,wpx*64,hs,xo).translated(0,yo);
        double e=md(s.outline(),12,(hs+63)/64+6);
        if(e<best){best=e;bx=xo;by=yo;}
      }
      printf("cubicArch w=%d hs=%d best=%.2f @(%d,%d)\n",wpx,hs,best,bx,by);
    }
  }
  return 0;
}
