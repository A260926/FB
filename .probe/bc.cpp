#include <cstdio>
#include <vector>
#include <cmath>
#include "raster_api.h"
#include "reference.h"
#include "fixtures.h"
int main(){
  for(int tr=0;tr<2;++tr){
    fx::Shape s=fx::bandsCurves().translated(tr?-13:0,tr?-19:0);
    FT_Outline o=s.outline();
    int W=80,H=200;
    std::vector<double> refc; 
    auto rr=ref::render(o,W,H,0,5e-5);
    for(int a=0;a<2;++a){
      const FT_Raster_Funcs* api=a?&ft_grays_raster_small:&ft_grays_raster;
      std::vector<unsigned char> g((size_t)W*H,0);
      FT_Bitmap bm{};bm.width=W;bm.rows=H;bm.pitch=W;bm.buffer=g.data();bm.num_grays=256;bm.pixel_mode=FT_PIXEL_MODE_GRAY;
      FT_Raster r;api->raster_new(nullptr,&r);FT_Raster_Params p{};p.source=&o;p.target=&bm;p.flags=FT_RASTER_FLAG_AA;
      api->raster_render(r,&p);api->raster_done(r);
      double mx=0;int bx=0,by=0,bg=0;double br=0;
      for(int y=0;y<H;++y)for(int x=0;x<W;++x){int gg=g[(size_t)(H-1-y)*W+x];double rv=rr.gray[(size_t)y*W+x];double d=std::fabs(gg-rv);if(d>mx){mx=d;bx=x;by=y;bg=gg;br=rv;}}
      printf("tr%d %s max=%.1f at(%d,%d) g=%d ref=%.0f\n",tr,a?"2K":"16K",mx,bx,by,bg,br);
    }
  }
  return 0;
}
