// Sweep conic/cubic arch fixtures; report max gray diff vs reference and
// the number of badly-lit pixels.  Goal: find gentle-but-genuine curves that
// stay within 4 gray.
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
struct Stat{ double mx; int over4; };
static Stat stats(const FT_Outline&o,int W,int H,int flags=0){
  std::vector<unsigned char>g;pixmap(o,W,H,g);
  std::vector<double>r;double me,ae;ref::render(o,W,H,flags,r,me,ae);
  Stat s{0,0};
  for(int y=0;y<H;++y)for(int x=0;x<W;++x){
    double d=std::fabs((double)g[(size_t)(H-1-y)*W+x]-r[(size_t)y*W+x]);
    if(d>s.mx)s.mx=d; if(d>4.0)s.over4++;
  }
  return s;
}
struct B{std::vector<FT_Vector>p;std::vector<char>t;short c;
  void add(FT_Vector v,char g){p.push_back(v);t.push_back(g);}
  FT_Outline out(){c=(short)(p.size()-1);FT_Outline o{};o.n_contours=1;o.n_points=(short)p.size();o.points=p.data();o.tags=t.data();o.contours=&c;return o;}
};

int main(){
  printf("conic arch: width w px, height h px, x offset frac, y base frac\n");
  int bestOver=1e9;
  for(int wp=2; wp<=8; ++wp)
   for(int hsub=4; hsub<=96; hsub+=4)
    for(int xf=0; xf<8; ++xf)
     for(int yf=0; yf<8; ++yf){
      int x0=64+yf*8, y0=64+xf*8; // swap to vary both
      int x1=x0+wp*64;
      B b; b.add({x0,y0},1); b.add({(x0+x1)/2, y0+hsub},0); b.add({x1,y0},1);
      int H=2+ (hsub+63)/64 + 2; int W=wp+4;
      Stat s=stats(b.out(),W,H);
      if(s.over4<bestOver){bestOver=s.over4;}
      if(s.mx<=3.0 && hsub>=16){
        printf("  w=%d h=%d/256px xf=%d yf=%d max=%.2f over4=%d\n",wp,hsub,xf,yf,s.mx,s.over4);
      }
    }
  printf("best over4 = %d\n",bestOver);
  return 0;
}
