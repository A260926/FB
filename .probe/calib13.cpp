// Map achievable rasterizer-vs-reference error for conic and cubic arches.
#include <cstdio>
#include <vector>
#include <cmath>
#include <algorithm>
#include "raster_api.h"
#include "reference.h"

static void pixmap(const FT_Outline&o,int W,int H,std::vector<unsigned char>&out){
  out.assign((size_t)W*H,0);FT_Bitmap bm{};bm.width=W;bm.rows=H;bm.pitch=W;bm.buffer=out.data();bm.num_grays=256;bm.pixel_mode=FT_PIXEL_MODE_GRAY;
  FT_Raster r;ft_grays_raster.raster_new(nullptr,&r);FT_Raster_Params p{};p.source=&o;p.target=&bm;p.flags=FT_RASTER_FLAG_AA;
  ft_grays_raster.raster_render(r,&p);ft_grays_raster.raster_done(r);
}
static double maxdiff(const FT_Outline&o,int W,int H){
  std::vector<unsigned char>g;pixmap(o,W,H,g);
  std::vector<double>r;double me,ae;ref::render(o,W,H,0,r,me,ae);
  double mx=0;for(int y=0;y<H;++y)for(int x=0;x<W;++x)
    mx=std::max(mx,std::fabs((double)g[(size_t)(H-1-y)*W+x]-r[(size_t)y*W+x]));
  return mx;
}
struct B{std::vector<FT_Vector>p;std::vector<char>t;short c;
  void add(FT_Vector v,char g){p.push_back(v);t.push_back(g);}
  FT_Outline out(){c=(short)(p.size()-1);FT_Outline o{};o.n_contours=1;o.n_points=(short)p.size();o.points=p.data();o.tags=t.data();o.contours=&c;return o;}
};

int main(){
  // For each (w,h) arch find best phase over x/y offsets at 1/8 px.
  printf("conic arches: wp px, h subpix (256=1px) -> best maxdiff over phases\n");
  for(int wp=3;wp<=10;++wp){
    for(int hs=16;hs<=320;hs+=16){
      double best=1e9; int bx=0,by=0; int W=wp+4, H=(hs+255)/256+4;
      for(int xf=0;xf<8;++xf)for(int yf=0;yf<8;++yf){
        int x0=64+xf*8, y0=64+yf*8, x1=x0+wp*64;
        B b; b.add({x0,y0},1); b.add({(x0+x1)/2, y0+hs},0); b.add({x1,y0},1);
        double d=maxdiff(b.out(),W,H);
        if(d<best){best=d;bx=xf;by=yf;}
      }
      printf("w=%2d h=%3d/256(%.2fpx) best=%5.2f at phase(%d,%d)%s\n",
             wp,hs,hs/256.0,best,bx,by, best<=4?"  <== FIT":"");
    }
  }
  return 0;
}
