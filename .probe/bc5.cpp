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
    auto rr=ref::render(o,W,H,0,5e-5);
    std::vector<unsigned char> g((size_t)W*H,0);
    FT_Bitmap bm{};bm.width=W;bm.rows=H;bm.pitch=W;bm.buffer=g.data();bm.num_grays=256;bm.pixel_mode=FT_PIXEL_MODE_GRAY;
    FT_Raster r;ft_grays_raster.raster_new(nullptr,&r);FT_Raster_Params p{};p.source=&o;p.target=&bm;p.flags=FT_RASTER_FLAG_AA;
    ft_grays_raster.raster_render(r,&p);ft_grays_raster.raster_done(r);
    int bad=0; for(int y=0;y<H;++y)for(int x=0;x<W;++x){double d=std::fabs((double)g[(size_t)(H-1-y)*W+x]-rr.gray[(size_t)y*W+x]);if(d>4){bad++;if(bad<8)printf("tr%d diff %.1f at(%d,%d) g%d r%d\n",tr,d,x,y,g[(size_t)(H-1-y)*W+x],(int)rr.gray[(size_t)y*W+x]);}}
    printf("tr%d total>4 = %d\n",tr,bad);
  }
  return 0;
}
