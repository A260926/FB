#include <cstdio>
#include <vector>
#include <cmath>
#include "raster_api.h"
#include "reference.h"
#include "fixtures.h"
int main(){
  fx::Shape s=fx::bandsCurves();
  FT_Outline o=s.outline();
  int W=80,H=200;
  auto rr=ref::render(o,W,H,0,5e-5);
  std::vector<unsigned char> g((size_t)W*H,0);
  FT_Bitmap bm{};bm.width=W;bm.rows=H;bm.pitch=W;bm.buffer=g.data();bm.num_grays=256;bm.pixel_mode=FT_PIXEL_MODE_GRAY;
  FT_Raster r;ft_grays_raster.raster_new(nullptr,&r);FT_Raster_Params p{};p.source=&o;p.target=&bm;p.flags=FT_RASTER_FLAG_AA;
  ft_grays_raster.raster_render(r,&p);ft_grays_raster.raster_done(r);
  // count diffs and print rows around y=38..42, x=10..20
  int nd=0; for(int y=0;y<H;++y)for(int x=0;x<W;++x)if(std::fabs((double)g[(size_t)(H-1-y)*W+x]-rr.gray[(size_t)y*W+x])>4)nd++;
  printf("diffs>4: %d\n",nd);
  for(int y=44;y>=34;--y){printf("y=%3d g:",y);for(int x=8;x<24;++x)printf("%4d",(int)g[(size_t)(H-1-y)*W+x]);printf("\n      r:");for(int x=8;x<24;++x)printf("%4d",(int)rr.gray[(size_t)y*W+x]);printf("\n");}
  return 0;
}
