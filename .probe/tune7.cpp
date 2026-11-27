// Multi-segment arch: force FreeType subdivision by chaining small cubics
// whose control offsets exceed ONE_PIXEL/2 in y.
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
    const FT_Raster_Funcs* api=a?&ft_grays_raster_small:&ft_grays_raster;
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
// multi-cubic arch: N cubics from (x0,yb) to (x1,yb), each peaked by hsub at
// midpoint; controls offset vertically by hc to force splits.
static fx::Shape multiCubicArch(int x0,int yb,int wpx,int N,int apexSub,int ctrlSub,int xo,int yo){
  fx::Shape s; s.on(x0+xo,yb+yo);
  for(int i=0;i<N;++i){
    int xa=x0+wpx*i/N, xb=x0+wpx*(i+1)/N, mid=(xa+xb)/2;
    int h = (i==0||i==N-1)? apexSub/2 : apexSub;  // first/last half-peak
    s.cubic(xa+ (xb-xa)/3 +xo, yb+ctrlSub+yo)
     .cubic(xa+2*(xb-xa)/3 +xo, yb+ctrlSub+yo)
     .cubic(mid+xo, yb+h+yo)  // extra control forces split
     .cubic(xb+ (xb-xa)/6 +xo, yb+ctrlSub+yo);
    s.on(xb+xo,yb+yo);
  }
  s.on(x0+wpx+xo,yb-yb+yo-64).on(x0+xo,yb-64+yo).end();
  return s;
}
int main(){
  int baseRow=40, yb=baseRow*64-20, W=12;
  for(int N : {1,2}) for(int apex : {160,200,240}) for(int ctrl : {40,80,120}) {
    double best=1e9;int bx=0,by=0;
    for(int xo=0;xo<64;xo+=4)for(int yo=0;yo<32;yo+=4){
      fx::Shape s=multiCubicArch(fx::px(2),yb,4*64,N,apex,ctrl,xo,yo);
      int H=(yb+apex+yo)/64+4;
      double e=md(s.outline(),W,H);
      if(e<best){best=e;bx=xo;by=yo;}
    }
    printf("multiCubic N=%d apex=%d ctrl=%d best=%.2f @(%d,%d)%s\n",N,apex,ctrl,best,bx,by,best<=3.5?"  FIT":"");
  }
  return 0;
}
