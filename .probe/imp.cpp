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
  // OnEnd variants: vary final closing point and offsets
  for(int mode=0;mode<3;++mode){
    double best=1e9;int bx=0,by=0,bw=0,bh=0;
    for(int W=9;W<=14;++W)for(int H=7;H<=12;++H)
      for(int dx=-8;dx<=8;dx+=4)for(int dy=-8;dy<=8;dy+=4){
        fx::Shape s;
        if(mode==0) s.on(fx::px(1),fx::px(3)).conic(fx::px(3),fx::px(4)).on(fx::px(5),fx::px(3)).on(fx::px(4),fx::px(1)).on(fx::px(2),fx::px(1)).end();
        else if(mode==1) s.on(fx::px(1),fx::px(2)).conic(fx::px(3),fx::px(3)).on(fx::px(5),fx::px(2)).on(fx::px(4),fx::px(1)).on(fx::px(2),fx::px(1)).end();
        else s.on(fx::px(1),fx::px(3)).conic(fx::px(3),fx::px(5)).on(fx::px(5),fx::px(3)).on(fx::px(4),fx::px(1)).on(fx::px(2),fx::px(1)).end();
        fx::Shape t=s.translated(dx,dy);
        double e=md(t.outline(),W,H);
        if(e<best){best=e;bx=dx;by=dy;bw=W;bh=H;}
      }
    printf("OnEnd mode=%d best=%.2f dx=%d dy=%d W=%d H=%d%s\n",mode,best,bx,by,bw,bh,best<=3.5?"  FIT":"");
  }
  // OffEnd variants
  for(int mode=0;mode<2;++mode){
    double best=1e9;int bx=0,by=0,bw=0,bh=0;
    for(int W=9;W<=14;++W)for(int H=7;H<=12;++H)
      for(int dx=-8;dx<=8;dx+=4)for(int dy=-8;dy<=8;dy+=4){
        fx::Shape s;
        if(mode==0) s.conic(fx::px(4),fx::px(1)).on(fx::px(6),fx::px(2)).conic(fx::px(5),fx::px(4)).on(fx::px(3),fx::px(4)).conic(fx::px(1),fx::px(2)).end();
        else s.conic(fx::px(4),fx::px(1)).on(fx::px(7),fx::px(2)).conic(fx::px(5),fx::px(4)).on(fx::px(3),fx::px(4)).conic(fx::px(1),fx::px(3)).end();
        fx::Shape t=s.translated(dx,dy);
        double e=md(t.outline(),W,H);
        if(e<best){best=e;bx=dx;by=dy;bw=W;bh=H;}
      }
    printf("OffEnd mode=%d best=%.2f dx=%d dy=%d W=%d H=%d%s\n",mode,best,bx,by,bw,bh,best<=3.5?"  FIT":"");
  }
  return 0;
}
