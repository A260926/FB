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
  for(int humps : {7,8}) for(int b : {24,28,32}) {
    double best=1e9;int bo=0,byo=0;
    for(int xo=0;xo<64;xo+=4)for(int yo=0;yo<64;yo+=4){
      double e=md(fx::tallCubicWave(humps,b,1,fx::px(42)+xo,yo).outline(),12,123);
      if(e<best){best=e;bo=xo;byo=yo;}
    }
    printf("cubicWave2 humps=%d b=%d best=%.2f xoff=%d yoff=%d\n",humps,b,best,fx::px(42)+bo,byo);
  }
  return 0;
}
