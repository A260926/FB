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
int main(){
  printf("OnEnd  10x104 = %.1f\n",md(fx::impliedConicStartOnEnd().outline(),10,104));
  printf("OffEnd 10x104 = %.1f\n",md(fx::impliedConicStartOffEnd().outline(),10,104));
  printf("conicWave 9x128 = %.1f\n",md(fx::tallConicWave(6,24,1,0,0).outline(),9,128));
  printf("cubicWave 16x128 = %.1f\n",md(fx::tallCubicWave(6,32,1,fx::px(9),0).outline(),16,128));
  return 0;
}
