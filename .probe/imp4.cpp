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
  // A large, low-curvature shape with convex sides built from off-curve
  // points; first and last points off-curve. Controls offset vertically by
  // ~2px, which forces subdivision.
  double best=1e9;int bx=0,by=0,bw=0,bh=0;
  for(int d : {200,240,280,320}) // control offset above on/off points
   for(int W=16;W<=24;++W)for(int H=16;H<=24;++H)
    for(int dx=-8;dx<=8;dx+=8)for(int dy=-8;dy<=8;dy+=8){
      fx::Shape s;
      // diamond-ish: offcurve at top, on right, offcurve lower-right, on bottom,
      // offcurve left, close (left off -> midpoint start)
      s.conic(fx::px(8),fx::px(2)).on(fx::px(14),fx::px(8)).conic(fx::px(14)+d,fx::px(14)).on(fx::px(8),fx::px(14)).conic(fx::px(2),fx::px(14)).end();
      fx::Shape t=s.translated(dx,dy);
      double e=md(t.outline(),W,H);
      if(e<best){best=e;bx=dx;by=dy;bw=W;bh=H;}
    }
  printf("OffEnd v3: best=%.2f dx=%d dy=%d W=%d H=%d%s\n",best,bx,by,bw,bh,best<=3.5?"  FIT":"");
  return 0;
}
