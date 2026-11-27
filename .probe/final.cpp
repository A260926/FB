// Final calibration: for each candidate fixture, report maxdiff at the exact
// fixed position that will be baked into fixtures.h, for both pools.
#include <cstdio>
#include <vector>
#include <cmath>
#include <algorithm>
#include "raster_api.h"
#include "reference.h"
#include "fixtures.h"

static double md( const FT_Raster_Funcs& api, const FT_Outline& o, int W,int H,int fl=0){
  std::vector<unsigned char> g((size_t)W*H,0);
  FT_Bitmap bm{};bm.width=W;bm.rows=H;bm.pitch=W;bm.buffer=g.data();bm.num_grays=256;bm.pixel_mode=FT_PIXEL_MODE_GRAY;
  FT_Raster r;api.raster_new(nullptr,&r);FT_Raster_Params p{};p.source=&o;p.target=&bm;p.flags=FT_RASTER_FLAG_AA;
  api.raster_render(r,&p);api.raster_done(r);
  auto rr=ref::render(o,W,H,fl,2e-4);double mx=0;int bx=0,by=0,gg=0;double rv=0;
  for(int y=0;y<H;++y)for(int x=0;x<W;++x){double d=std::fabs((double)g[(size_t)(H-1-y)*W+x]-rr.gray[(size_t)y*W+x]);if(d>mx){mx=d;bx=x;by=y;gg=g[(size_t)(H-1-y)*W+x];rv=rr.gray[(size_t)y*W+x];}}
  (void)bx;(void)by;(void)gg;(void)rv;
  return mx;
}

int main(){
  const FT_Raster_Funcs* ap[2]={&ft_grays_raster,&ft_grays_raster_small};
  const char* nm[2]={"16K","2K"};
  auto rep=[&](const char*name,fx::Shape s,int W,int H,int fl=0){
    FT_Outline o=s.outline(fl);
    auto rr=ref::render(o,W,H,fl,2e-4);
    printf("%-26s refErr=%.2g areaErr=%.3g",name,rr.maxErr,rr.areaErr);
    for(int a=0;a<2;++a)printf("  %s=%.2f",nm[a],md(*ap[a],o,W,H,fl));
    printf("\n");
  };
  printf("=== bands ===\n");
  rep("bandsPressure",fx::bandsPressure(),100,166);
  rep("bandsCurves",fx::bandsCurves(),80,104);
  printf("=== curves ===\n");
  rep("impliedOnEnd",fx::impliedConicStartOnEnd(),8,6);
  rep("impliedOffEnd",fx::impliedConicStartOffEnd(),8,6);
  rep("tallConic",fx::tallConic(),8,103);
  rep("tallCubic",fx::tallCubic(),8,103);
  rep("conicExtremum",fx::conicExtremum(),9,9);
  rep("cubicExtremum",fx::cubicExtremum(),9,9);
  rep("innerLoop same",fx::innerLoop(false),14,14);
  rep("innerLoop rev",fx::innerLoop(true),14,14);
  rep("innerLoop same EO",fx::innerLoop(false),14,14,ref::evenOddFlag());
  rep("innerLoop rev EO",fx::innerLoop(true),14,14,ref::evenOddFlag());
  rep("ring",fx::ring(),15,12);
  rep("ring EO",fx::ring(),15,12,ref::evenOddFlag());
  rep("touchingEdge",fx::touchingEdge(),12,9);
  rep("touchingCorner",fx::touchingCorner(),12,12);
  rep("duplicateRect NZ",fx::duplicateRect(),12,12);
  rep("duplicateRect EO",fx::duplicateRect(),12,12,ref::evenOddFlag());
  rep("layoutMixed",fx::layoutMixed(),18,12);
  rep("negativeShape",fx::negativeShape(),10,8);
  return 0;
}
