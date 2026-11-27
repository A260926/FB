// Print grid of rasterizer vs exact clipped area for triangle & quad.
#include <cstdio>
#include <vector>
#include <cmath>
#include <algorithm>
#include "raster_api.h"

struct P{double x,y;};
static std::vector<P> clipRect(std::vector<P> poly,double x0,double y0,double x1,double y1){
  auto c1=[&](std::vector<P> in,int ax,double v,bool gt){
    std::vector<P> o; if(in.empty())return o;
    for(size_t i=0;i<in.size();++i){P a=in[i],b=in[(i+1)%in.size()];
      double va=ax? a.y:a.x, vb=ax? b.y:b.x;
      bool ia=gt?va>=v:va<=v, ib=gt?vb>=v:vb<=v;
      if(ia)o.push_back(a);
      if(ia!=ib){double t=(v-va)/(vb-va);o.push_back({a.x+t*(b.x-a.x),a.y+t*(b.y-a.y)});}}
    return o;};
  poly=c1(poly,0,x0,true);poly=c1(poly,0,x1,false);poly=c1(poly,1,y0,true);poly=c1(poly,1,y1,false);
  return poly;
}
static double area(const std::vector<P>&p){double a=0;for(size_t i=0;i<p.size();++i){auto u=p[i],v=p[(i+1)%p.size()];a+=u.x*v.y-v.x*u.y;}return std::fabs(a)/2;}

static void pixmap(const FT_Outline&o,int W,int H,std::vector<unsigned char>&out){
  out.assign((size_t)W*H,0);FT_Bitmap bm{};bm.width=W;bm.rows=H;bm.pitch=W;bm.buffer=out.data();bm.num_grays=256;bm.pixel_mode=FT_PIXEL_MODE_GRAY;
  FT_Raster r;ft_grays_raster.raster_new(nullptr,&r);FT_Raster_Params p{};p.source=&o;p.target=&bm;p.flags=FT_RASTER_FLAG_AA;
  ft_grays_raster.raster_render(r,&p);ft_grays_raster.raster_done(r);
}

static void grid(const char*name,std::vector<P>poly,const FT_Outline&o,int W,int H){
  std::vector<unsigned char>got;pixmap(o,W,H,got);
  printf("--- %s ---\n",name);
  for(int y=H-1;y>=0;--y){
    for(int x=0;x<W;++x){
      auto q=clipRect(poly,x,y,x+1,y+1);double a=area(q)*255.0;
      printf("%3d/%5.1f ",(int)got[(size_t)(H-1-y)*W+x],a);
    }printf("\n");
  }
}

int main(){
  std::vector<P> tri={{1,1},{5,1},{1,4}};
  FT_Vector p[3]={{64,64},{320,64},{64,256}};char t[3]={1,1,1};short c=2;
  FT_Outline o{};o.n_contours=1;o.n_points=3;o.points=p;o.tags=t;o.contours=&c;
  grid("triangle",tri,o,7,6);
  std::vector<P> quad={{80.0/64,48.0/64},{320.0/64,96.0/64},{288.0/64,288.0/64},{48.0/64,240.0/64}};
  FT_Vector p2[4]={{80,48},{320,96},{288,288},{48,240}};char t2[4]={1,1,1,1};short c2=3;
  FT_Outline o2{};o2.n_contours=1;o2.n_points=4;o2.points=p2;o2.tags=t2;o2.contours=&c2;
  grid("quad",quad,o2,7,6);
  return 0;
}
