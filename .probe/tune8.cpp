// Proper cubic arch: single cubic per arch; y-control offset chosen to force
// subdivision (|2*P0 - 3*P1 + P3| > 0.5 px) while keeping curvature gentle.
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
// arch: start (x0,yb), c1 (x0+w/3, yb+cy1), c2 (x0+2w/3, yb+cy2), end (x0+w,yb)
static fx::Shape arch(int x0,int yb,int w,int cy1,int cy2,int xo,int yo){
  fx::Shape s; s.on(x0+xo,yb+yo)
    .cubic(x0+w/3+xo,yb+cy1+yo).cubic(x0+2*w/3+xo,yb+cy2+yo)
    .on(x0+w+xo,yb+yo).on(x0+w+xo,yb-64+yo).on(x0+xo,yb-64+yo).end();
  return s;
}
int main(){
  int baseRow=40, yb=baseRow*64-20, W=12, wpx=4;
  for(int cy1 : {120,160,200,240,280}) for(int cy2 : {120,160,200,240,280}) {
    double best=1e9;int bx=0,by=0;
    for(int xo=0;xo<64;xo+=4)for(int yo=0;yo<32;yo+=4){
      fx::Shape s=arch(fx::px(2),yb,wpx*64,cy1,cy2,xo,yo);
      int H=(yb+std::max(cy1,cy2)+yo)/64+4;
      double e=md(s.outline(),W,H);
      if(e<best){best=e;bx=xo;by=yo;}
    }
    printf("arch cy1=%d cy2=%d best=%.2f @(%d,%d)%s\n",cy1,cy2,best,bx,by,best<=3.5?"  FIT":"");
  }
  return 0;
}
