// Axis-aligned fractional rectangles: exact area vs rasterizer gray.
#include <cstdio>
#include <cmath>
#include <algorithm>
#include "raster_api.h"

static void pixmap(const FT_Outline&o,int W,int H,unsigned char*out){
  for(int i=0;i<W*H;++i)out[i]=0;
  FT_Bitmap bm{};bm.width=W;bm.rows=H;bm.pitch=W;bm.buffer=out;bm.num_grays=256;bm.pixel_mode=FT_PIXEL_MODE_GRAY;
  FT_Raster r;ft_grays_raster.raster_new(nullptr,&r);FT_Raster_Params p{};p.source=&o;p.target=&bm;p.flags=FT_RASTER_FLAG_AA;
  ft_grays_raster.raster_render(r,&p);ft_grays_raster.raster_done(r);
}

int main(){
  double worst256=0,worst255=0,worst256r=0;
  int wcase=0;
  double wa=0,wg=0,wr256=0;
  for(int a=1;a<64;a+=1) for(int b=1;b<64;b+=1){
    int x0=a, y0=b, x1=a+64, y1=b+64; // exactly 1px wide/tall rectangle at fractional offset
    // make 2x2 rectangle: x0=a,x1=a+128? keep 1x1
    FT_Vector pts[4]={{x0,y0},{x1,y0},{x1,y1},{x0,y1}};
    char tg[4]={1,1,1,1}; short ct=3;
    FT_Outline o{};o.n_contours=1;o.n_points=4;o.points=pts;o.tags=tg;o.contours=&ct;
    int W=4,H=4; unsigned char buf[16]; pixmap(o,W,H,buf);
    double d=1.0-((double)a/64.0);  // covered width in left pixel
    double e=1.0-((double)b/64.0);
    for(int y=0;y<H;++y)for(int x=0;x<W;++x){
      double area=0;
      if(x==0&&y==0) area=d*e;
      else if(x==0&&(y==1||y==0&&false)) {}
      if(y==1){
        // full-height pixel? rectangle y in [b/64, b/64+1]; pixel row 1 = [1,2]
        // bottom row 0 partial e; row1: covers [1, 1+e]; row0 covered [b/64,1]
      }
      // compute generically
      double lx=std::max((double)x, (double)a/64.0), hx=std::min((double)(x+1),(double)a/64.0+1.0);
      double ly=std::max((double)y, (double)b/64.0), hy=std::min((double)(y+1),(double)b/64.0+1.0);
      double A=std::max(0.0,hx-lx)*std::max(0.0,hy-ly);
      int g=buf[(H-1-y)*W+x];
      double e256=std::fabs(g-A*256.0), e255=std::fabs(g-A*255.0), e256r=std::fabs(g-std::round(A*256.0));
      if(e256>worst256){worst256=e256;wcase=0;wa=A;wg=g;wr256=A*256.0;}
      worst255=std::max(worst255,e255); worst256r=std::max(worst256r,e256r);
    }
  }
  printf("1x1 frac rect: worst |g-A*256|=%.3f (A=%.4f*256=%.2f g=%d)\n",worst256,wa,wr256,wg);
  printf("               worst |g-A*255|=%.3f  worst |g-round(A*256)|=%.3f\n",worst255,worst256r);
  return 0;
}
