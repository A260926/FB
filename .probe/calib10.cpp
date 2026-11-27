// Calibrate the gray quantization convention on many simple straight shapes.
#include <cstdio>
#include <vector>
#include <cmath>
#include <algorithm>
#include "raster_api.h"
#include "reference.h"

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
  poly=c1(poly,0,x0,true);poly=c1(poly,0,x1,false);poly=c1(poly,1,y0,true);poly=c1(poly,1,y1,false);return poly;
}
static double area(const std::vector<P>&p){double a=0;for(size_t i=0;i<p.size();++i){auto u=p[i],v=p[(i+1)%p.size()];a+=u.x*v.y-v.x*u.y;}return std::fabs(a)/2;}

static void pixmap(const FT_Outline&o,int W,int H,std::vector<unsigned char>&out){
  out.assign((size_t)W*H,0);FT_Bitmap bm{};bm.width=W;bm.rows=H;bm.pitch=W;bm.buffer=out.data();bm.num_grays=256;bm.pixel_mode=FT_PIXEL_MODE_GRAY;
  FT_Raster r;ft_grays_raster.raster_new(nullptr,&r);FT_Raster_Params p{};p.source=&o;p.target=&bm;p.flags=FT_RASTER_FLAG_AA;
  ft_grays_raster.raster_render(r,&p);ft_grays_raster.raster_done(r);
}

int main(){
  double worst255=0, worst256=0, worst256r=0, worstRef=0;
  // scan sheared rectangles and triangles on a 16x16 grid with fractional offsets
  for(int m=0;m<200;++m){
    int W=12,H=12;
    // polygon: convex quad with fractional coords
    double x0=0.2+0.03*m, y0=0.25+0.017*m; // drift
    x0=std::fmod(x0,4.0); y0=std::fmod(y0,4.0);
    double x1=x0+2.3+0.05*(m%7), y1=y0+1.7+0.03*(m%5);
    double sh=0.15+0.04*(m%9);
    std::vector<P> poly={{x0,y0},{x1,y0+sh},{x1-sh,y1},{x0,y1-sh}};
    if(poly[0].x<0||poly[0].y<0||poly[2].x>=W||poly[2].y>=H) continue;
    std::vector<FT_Vector> pts;
    for(auto&q:poly)pts.push_back({(FT_Pos)llround(q.x*64),(FT_Pos)llround(q.y*64)});
    std::vector<char> tags(pts.size(),1); short ct=(short)pts.size()-1;
    FT_Outline o{};o.n_contours=1;o.n_points=(short)pts.size();o.points=pts.data();o.tags=tags.data();o.contours=&ct;
    std::vector<unsigned char> got;pixmap(o,W,H,got);
    // exact device polygon from the *rounded* integer coords (what raster sees)
    std::vector<P> dev; for(auto&q:pts)dev.push_back({q.x/64.0,q.y/64.0});
    std::vector<double> refc;double me,ae;ref::render(o,W,H,0,refc,me,ae);
    for(int y=0;y<H;++y)for(int x=0;x<W;++x){
      auto q=clipRect(dev,x,y,x+1,y+1); double A=area(q);
      int g=got[(size_t)(H-1-y)*W+x];
      double d255=std::fabs(g-A*255.0);
      double d256=std::fabs(g-A*256.0);
      double d256r=std::fabs(g-std::round(A*256.0));
      double dr=std::fabs(g-refc[(size_t)y*W+x]);
      worst255=std::max(worst255,d255); worst256=std::max(worst256,d256);
      worst256r=std::max(worst256r,d256r); worstRef=std::max(worstRef,dr);
    }
  }
  printf("worst |got-area*255|=%.3f\n",worst255);
  printf("worst |got-area*256|=%.3f\n",worst256);
  printf("worst |got-round(area*256)|=%.3f\n",worst256r);
  printf("worst |got-ref(256)|=%.3f\n",worstRef);
  return 0;
}
