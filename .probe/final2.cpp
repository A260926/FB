#include <cstdio>
#include <vector>
#include <cmath>
#include <algorithm>
#include "raster_api.h"
#include "reference.h"
#include "fixtures.h"
static const FT_Raster_Funcs* ap[2]={&ft_grays_raster,&ft_grays_raster_small};
static const char* nm[2]={"16K","2K"};
static double md(const FT_Outline&o,int W,int H,int fl=0){
  double worst=0;
  for(int a=0;a<2;++a){
    std::vector<unsigned char> g((size_t)W*H,0);
    FT_Bitmap bm{};bm.width=W;bm.rows=H;bm.pitch=W;bm.buffer=g.data();bm.num_grays=256;bm.pixel_mode=FT_PIXEL_MODE_GRAY;
    FT_Raster r;ap[a]->raster_new(nullptr,&r);FT_Raster_Params p{};p.source=&o;p.target=&bm;p.flags=FT_RASTER_FLAG_AA;
    ap[a]->raster_render(r,&p);ap[a]->raster_done(r);
    auto rr=ref::render(o,W,H,fl,1e-4);double mx=0;int bx=0,by=0;
    for(int y=0;y<H;++y)for(int x=0;x<W;++x){double d=std::fabs((double)g[(size_t)(H-1-y)*W+x]-rr.gray[(size_t)y*W+x]);if(d>mx){mx=d;bx=x;by=y;}}
    if(mx>worst)worst=mx;
    (void)bx;(void)by;
  }
  return worst;
}
int main(){
  auto rep=[&](const char*name,fx::Shape s,int W,int H,int fl=0){
    FT_Outline o=s.outline(fl); auto rr=ref::render(o,W,H,fl,1e-4);
    printf("%-24s refErr=%.2g area=%.3g  worst=%.2f\n",name,rr.maxErr,rr.areaErr,md(o,W,H,fl));
  };
  rep("bandsPressure",fx::bandsPressure(),100,166);
  rep("bandsCurves",fx::bandsCurves(),60,128);
  rep("impliedOnEnd",fx::impliedConicStartOnEnd(),8,6);
  rep("impliedOffEnd",fx::impliedConicStartOffEnd(),8,6);
  rep("tallConicWave",fx::tallConicWave(6,24,1,0,0),8,123);
  rep("tallCubicWave",fx::tallCubicWave(6,32,1,fx::px(9),0),12,123);
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
