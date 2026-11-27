#include <cstdio>
#include <vector>
#include "raster_api.h"
#include "reference.h"
#include "fixtures.h"
static void info(const char*nm,fx::Shape s,int W,int H,int fl=0){
  FT_Outline o=s.outline(fl);
  printf("%-22s npts=%d ncon=%d",nm,o.n_points,o.n_contours);
  std::vector<unsigned char> g((size_t)W*H,0);
  FT_Bitmap bm{};bm.width=W;bm.rows=H;bm.pitch=W;bm.buffer=g.data();bm.num_grays=256;bm.pixel_mode=FT_PIXEL_MODE_GRAY;
  FT_Raster r;ft_grays_raster.raster_new(nullptr,&r);
  FT_Raster_Params p{};p.source=&o;p.target=&bm;p.flags=FT_RASTER_FLAG_AA;
  int e=ft_grays_raster.raster_render(r,&p);ft_grays_raster.raster_done(r);
  int nz=0;for(auto v:g)if(v)nz++;
  auto rr=ref::render(o,W,H,fl);int rn=0;for(double v:rr.gray)if(v>0)rn++;
  printf(" err=%d raster_nz=%d ref_nz=%d\n",e,nz,rn);
  if(nm[0]=='d'){for(int y=0;y<H;++y){for(int x=0;x<W;++x)printf("%3d ",(int)g[(size_t)(H-1-y)*W+x]);printf("\n");}}
}
int main(){
  info("innerLoop same",fx::innerLoop(false),14,14);
  info("innerLoop rev",fx::innerLoop(true),14,14);
  info("ring",fx::ring(),15,12);
  info("touchingEdge",fx::touchingEdge(),12,9);
  info("touchingCorner",fx::touchingCorner(),12,12);
  info("duplicateRect",fx::duplicateRect(),12,12);
  info("duplicateRectEO",fx::duplicateRect(),12,12,ref::evenOddFlag());
  info("layoutMixed",fx::layoutMixed(),18,12);
  info("negative",fx::negativeShape(),10,8);
  info("implOnEnd",fx::impliedConicStartOnEnd(),8,6);
  return 0;
}
