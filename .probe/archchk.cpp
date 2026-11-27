#include <cstdio>
#include <vector>
#include "raster_api.h"
#include "reference.h"
#include "fixtures.h"
int main(){
  fx::Shape s=fx::conicArch(fx::px(2),40*64-20,4*64,160,0);
  FT_Outline o=s.outline();
  printf("npts=%d ncur=%d\n",o.n_points,o.n_contours);
  for(int i=0;i<o.n_points;++i)printf("  (%ld,%ld) tag%d\n",(long)o.points[i].x,(long)o.points[i].y,(int)o.tags[i]);
  int W=12,H=8;
  std::vector<unsigned char> g((size_t)W*H,0);
  FT_Bitmap bm{};bm.width=W;bm.rows=H;bm.pitch=W;bm.buffer=g.data();bm.num_grays=256;bm.pixel_mode=FT_PIXEL_MODE_GRAY;
  FT_Raster r;ft_grays_raster.raster_new(nullptr,&r);FT_Raster_Params p{};p.source=&o;p.target=&bm;p.flags=FT_RASTER_FLAG_AA;
  int e=ft_grays_raster.raster_render(r,&p);ft_grays_raster.raster_done(r);
  auto rr=ref::render(o,W,H,0,1e-4);int nz=0;for(auto v:g)if(v)nz++;
  printf("err=%d nz=%d refarea=%.2f\n",e,nz,rr.areaErr);
  for(int y=H-1;y>=0;--y){for(int x=0;x<W;++x)printf("%3d ",(int)g[(size_t)(H-1-y)*W+x]);printf("\n");}
  return 0;
}
