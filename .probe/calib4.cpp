// Calibrate quantization convention with axis-aligned fractional rectangle.
#include <cstdio>
#include <vector>
#include <cmath>
#include "raster_api.h"
#include "reference.h"

static void pixmap( const FT_Outline& o, int W, int H, std::vector<unsigned char>& out )
{
  out.assign( (size_t)W * H, 0 );
  FT_Bitmap bm{}; bm.width=(unsigned)W; bm.rows=(unsigned)H; bm.pitch=W;
  bm.buffer=out.data(); bm.num_grays=256; bm.pixel_mode=FT_PIXEL_MODE_GRAY;
  FT_Raster r; ft_grays_raster.raster_new(nullptr,&r);
  FT_Raster_Params p{}; p.source=&o; p.target=&bm; p.flags=FT_RASTER_FLAG_AA;
  ft_grays_raster.raster_render(r,&p);
  ft_grays_raster.raster_done(r);
}

static void rect( int x0,int y0,int x1,int y1, FT_Vector p[4], char tg[4], short& c )
{ p[0]={x0,y0}; p[1]={x1,y0}; p[2]={x1,y1}; p[3]={x0,y1}; tg[0]=tg[1]=tg[2]=tg[3]=1; c=3; }

int main()
{
  const int W=5,H=5;
  // 26.6 coords: x .4 .. 2.6 => 25.6 .. 166.4 -> integers 26,166
  FT_Vector p[4]; char tg[4]; short c; rect(26,26,166,166,p,tg,c);
  FT_Outline o{}; o.n_contours=1;o.n_points=4;o.points=p;o.tags=tg;o.contours=&c;
  std::vector<unsigned char> got; pixmap(o,W,H,got);
  std::vector<double> refc; double me,ae; ref::render(o,W,H,0,refc,me,ae);
  // analytic: x0=26/64=0.40625, x1=166/64=2.59375
  double ax0=26.0/64, ax1=166.0/64, ay0=26.0/64, ay1=166.0/64;
  printf("rect x[%.3f,%.3f] y[%.3f,%.3f]\n",ax0,ax1,ay0,ay1);
  double mxd=0; int px=0,py=0; double mg=0, mr=0;
  for(int r=0;r<H;++r){
    for(int x=0;x<W;++x){
      double lx=std::max((double)x,ax0), hx=std::min((double)(x+1),ax1);
      double ly=std::max((double)r,ay0), hy=std::min((double)(r+1),ay1);
      double area=std::max(0.0,hx-lx)*std::max(0.0,hy-ly);
      int a=got[(size_t)(H-1-r)*W+x];
      double b=refc[(size_t)r*W+x];
      printf("(%d,%d) got=%3d ref=%6.2f analytic=%6.2f\n",x,r,a,b,area*255.0);
      double d=std::fabs(a-area*255.0);
      if(d>mxd){mxd=d;px=x;py=r;mg=a;mr=area*255.0;}
    }
  }
  printf("max diff vs analytic = %.2f at (%d,%d) got=%g an=%.2f\n",mxd,px,py,mg,mr);
  return 0;
}
