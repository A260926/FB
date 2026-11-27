#include <cstdio>
#include <vector>
#include <cmath>
#include "raster_api.h"
#include "reference.h"
#include "fixtures.h"
static double md(const FT_Outline&o,int W,int H){
  double worst=0;
  for(int a=0;a<2;++a){const FT_Raster_Funcs*api=a?&ft_grays_raster_small:&ft_grays_raster;
    std::vector<unsigned char> g((size_t)W*H,0);
    FT_Bitmap bm{};bm.width=W;bm.rows=H;bm.pitch=W;bm.buffer=g.data();bm.num_grays=256;bm.pixel_mode=FT_PIXEL_MODE_GRAY;
    FT_Raster r;api->raster_new(nullptr,&r);FT_Raster_Params p{};p.source=&o;p.target=&bm;p.flags=FT_RASTER_FLAG_AA;
    api->raster_render(r,&p);api->raster_done(r);
    auto rr=ref::render(o,W,H,0,5e-5);double mx=0;
    for(int y=0;y<H;++y)for(int x=0;x<W;++x)mx=std::max(mx,std::fabs((double)g[(size_t)(H-1-y)*W+x]-rr.gray[(size_t)y*W+x]));
    worst=std::max(worst,mx);
  }
  return worst;
}
// Tall shape with wavy conic right boundary, but first contour point is an
// off-curve conic point (implicit start).  H=100px.
fx::Shape impliedWave(int humps,int b,int xo,int yo){
  fx::Shape s; int L=fx::px(1),R=fx::px(6),H=100*64;
  // first point off-curve (control near left-bottom), then on-curve
  s.conic(L+xo,yo); // off-curve start
  s.on(R+xo,yo);
  for(int i=0;i<humps;++i){int y0=H*i/humps,y1=H*(i+1)/humps;int sg=(i%2)?-1:1;
    s.conic(R+sg*b+xo,y0+(y1-y0)/4+yo).on(R+xo,y1+yo);}
  s.on(L+xo,H+yo).end();
  return s;
}
int main(){
  double best=1e9;int bx=0,by=0;
  for(int humps: {5,6,7})for(int b: {20,24,28})
   for(int xo=0;xo<64;xo+=8)for(int yo=0;yo<64;yo+=8){
     double e=md(impliedWave(humps,b,xo,yo).outline(),10,103);
     if(e<best){best=e;bx=xo;by=yo;}
   }
  printf("impliedWave best=%.2f xo=%d yo=%d%s\n",best,bx,by,best<=3.5?"  FIT":"");
  return 0;
}
