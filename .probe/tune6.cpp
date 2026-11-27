// Correct phase search: bitmap spans the arch's actual base row.
#include <cstdio>
#include <vector>
#include <cmath>
#include <algorithm>
#include "raster_api.h"
#include "reference.h"
#include "fixtures.h"

static double md(const FT_Outline&o,int W,int H,int fl=0){
  double worst=0;
  for(int a=0;a<2;++a){
    const FT_Raster_Funcs* api = a? &ft_grays_raster_small : &ft_grays_raster;
    std::vector<unsigned char> g((size_t)W*H,0);
    FT_Bitmap bm{};bm.width=W;bm.rows=H;bm.pitch=W;bm.buffer=g.data();bm.num_grays=256;bm.pixel_mode=FT_PIXEL_MODE_GRAY;
    FT_Raster r;api->raster_new(nullptr,&r);FT_Raster_Params p{};p.source=&o;p.target=&bm;p.flags=FT_RASTER_FLAG_AA;
    api->raster_render(r,&p);api->raster_done(r);
    auto rr=ref::render(o,W,H,fl,1e-4);double mx=0;
    for(int y=0;y<H;++y)for(int x=0;x<W;++x)mx=std::max(mx,std::fabs((double)g[(size_t)(H-1-y)*W+x]-rr.gray[(size_t)y*W+x]));
    worst=std::max(worst,mx);
  }
  return worst;
}

int main(){
  // arches at a given base row; bitmap tall enough.
  for(int baseRow : {10,40,81}) {
    for(int kind=0;kind<2;++kind){
      for(int wpx : {4,5,6}) for(int hs : {120,160,200}) {
        double best=1e9;int bx=0,by=0;
        for(int xo=0;xo<64;xo+=4)for(int yo=0;yo<32;yo+=4){
          int yb=baseRow*64-20;
          fx::Shape s = kind? fx::cubicArch(fx::px(2),yb,wpx*64,hs,xo)
                            : fx::conicArch(fx::px(2),yb,wpx*64,hs,xo);
          s=s.translated(0,yo);
          int W=12, H=(yb+hs+yo)/64+4;
          double e=md(s.outline(),W,H);
          if(e<best){best=e;bx=xo;by=yo;}
        }
        printf("%s base=%d w=%d hs=%d best=%.2f @(%d,%d)%s\n",
               kind?"cubic":"conic",baseRow,wpx,hs,best,bx,by,best<=3.5?"  FIT":"");
      }
    }
  }
  return 0;
}
