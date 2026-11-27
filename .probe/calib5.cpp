// Exact per-pixel area by Sutherland-Hodgman polygon clipping (double),
// compared against the rasterizer output, to calibrate the area convention.
#include <cstdio>
#include <vector>
#include <cmath>
#include <algorithm>
#include "raster_api.h"

struct P{double x,y;};

static std::vector<P> clipRect(std::vector<P> poly,double x0,double y0,double x1,double y1){
  auto clip1=[&](std::vector<P> in,int axis,double v,bool keepGreater){
    std::vector<P> out; if(in.empty())return out;
    for(size_t i=0;i<in.size();++i){
      P a=in[i],b=in[(i+1)%in.size()];
      double va=axis==0?a.x:a.y, vb=axis==0?b.x:b.y;
      bool ina=keepGreater?va>=v:va<=v, inb=keepGreater?vb>=v:vb<=v;
      if(ina) out.push_back(a);
      if(ina!=inb){
        double t=(v-va)/(vb-va);
        out.push_back({a.x+t*(b.x-a.x),a.y+t*(b.y-a.y)});
      }
    }
    return out;
  };
  poly=clip1(poly,0,x0,true); poly=clip1(poly,0,x1,false);
  poly=clip1(poly,1,y0,true); poly=clip1(poly,1,y1,false);
  return poly;
}
static double polyArea(const std::vector<P>& p){double a=0;for(size_t i=0;i<p.size();++i){auto u=p[i],v=p[(i+1)%p.size()];a+=u.x*v.y-v.x*u.y;}return std::fabs(a)/2;}

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

int main(){
  // triangle (1,1)->(5,1)->(1,4)
  std::vector<P> poly={{1,1},{5,1},{1,4}};
  FT_Vector p[3]={{64,64},{320,64},{64,256}}; char t[3]={1,1,1}; short c=2;
  FT_Outline o{};o.n_contours=1;o.n_points=3;o.points=p;o.tags=t;o.contours=&c;
  int W=7,H=6; std::vector<unsigned char> got; pixmap(o,W,H,got);
  double mx255=0,mx256=0; int x255=0,y255=0,g255=0,a255=0,x256=0,y256=0,g256=0,a256=0;
  for(int y=0;y<H;++y)for(int x=0;x<W;++x){
    auto q=clipRect(poly,x,y,x+1,y+1); double ar=polyArea(q);
    int g=got[(size_t)(H-1-y)*W+x];
    double d255=std::fabs(g-ar*255.0), d256=std::fabs(g-ar*256.0);
    if(d255>mx255){mx255=d255;x255=x;y255=y;g255=g;a255=ar*255.0;}
    if(d256>mx256){mx256=d256;x256=x;y256=y;g256=g;a256=ar*256.0;}
  }
  printf("tri vs 255-scale: max=%.3f at(%d,%d) got=%d an=%.2f\n",mx255,x255,y255,g255,a255);
  printf("tri vs 256-scale: max=%.3f at(%d,%d) got=%d an=%.2f\n",mx256,x256,y256,g256,a256);

  // slanted quad (80,48),(320,96),(288,288),(48,240) in 26.6 -> device
  std::vector<P> q={{80.0/64,48.0/64},{320.0/64,96.0/64},{288.0/64,288.0/64},{48.0/64,240.0/64}};
  FT_Vector p2[4]={{80,48},{320,96},{288,288},{48,240}};char t2[4]={1,1,1,1};short c2=3;
  FT_Outline o2{};o2.n_contours=1;o2.n_points=4;o2.points=p2;o2.tags=t2;o2.contours=&c2;
  std::vector<unsigned char> got2; pixmap(o2,W,H,got2);
  mx255=mx256=0;
  for(int y=0;y<H;++y)for(int x=0;x<W;++x){
    auto qq=clipRect(q,x,y,x+1,y+1); double ar=polyArea(qq);
    int g=got2[(size_t)(H-1-y)*W+x];
    double d255=std::fabs(g-ar*255.0), d256=std::fabs(g-ar*256.0);
    if(d255>mx255){mx255=d255;x255=x;y255=y;g255=g;a255=ar*255.0;}
    if(d256>mx256){mx256=d256;x256=x;y256=y;g256=g;a256=ar*256.0;}
  }
  printf("quad vs 255-scale: max=%.3f at(%d,%d) got=%d an=%.2f\n",mx255,x255,y255,g255,a255);
  printf("quad vs 256-scale: max=%.3f at(%d,%d) got=%d an=%.2f\n",mx256,x256,y256,g256,a256);
  return 0;
}
