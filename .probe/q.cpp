#include <cstdio>
#include <vector>
#include <cmath>
#include <algorithm>
#include "raster_api.h"
#include "reference.h"
#include "fixtures.h"
struct P{double x,y;};
static std::vector<P> clip(std::vector<P> poly,double x0,double y0,double x1,double y1){
  auto c1=[&](std::vector<P> in,int ax,double v,bool gt){std::vector<P>o;if(in.empty())return o;
    for(size_t i=0;i<in.size();++i){P a=in[i],b=in[(i+1)%in.size()];double va=ax?a.y:a.x,vb=ax?b.y:b.x;bool ia=gt?va>=v:va<=v,ib=gt?vb>=v:vb<=v;if(ia)o.push_back(a);if(ia!=ib){double t=(v-va)/(vb-va);o.push_back({a.x+t*(b.x-a.x),a.y+t*(b.y-a.y)});}}return o;};
  auto t1=c1(poly,0,x0,true);t1=c1(t1,0,x1,false);t1=c1(t1,1,y0,true);return c1(t1,1,y1,false);
}
static double area(const std::vector<P>&p){double a=0;for(size_t i=0;i<p.size();++i){auto u=p[i],v=p[(i+1)%p.size()];a+=u.x*v.y-v.x*u.y;}return std::fabs(a)/2;}
int main(){
  // bandsPressure (all straight)
  fx::Shape s=fx::bandsPressure();
  FT_Outline o=s.outline();
  int W=100,H=166;
  std::vector<unsigned char> g((size_t)W*H,0);
  FT_Bitmap bm{};bm.width=W;bm.rows=H;bm.pitch=W;bm.buffer=g.data();bm.num_grays=256;bm.pixel_mode=FT_PIXEL_MODE_GRAY;
  FT_Raster r;ft_grays_raster.raster_new(nullptr,&r);
  FT_Raster_Params p{};p.source=&o;p.target=&bm;p.flags=FT_RASTER_FLAG_AA;
  ft_grays_raster.raster_render(r,&p);ft_grays_raster.raster_done(r);
  // build polygon list from reference to clip per pixel precisely (non-zero winding)
  // Instead use reference gray; also compute floor model diff.
  auto rr=ref::render(o,W,H,0);
  double maxfloor=0,maxraw=0;int fx=0,fy=0;
  for(int y=0;y<H;++y)for(int x=0;x<W;++x){
    int gg=g[(size_t)(H-1-y)*W+x];
    double v=rr.gray[(size_t)y*W+x];
    double d1=std::fabs(gg-std::floor(v+1e-9));
    double d2=std::fabs(gg-v);
    if(d1>maxfloor){maxfloor=d1;fx=x;fy=y;}
    maxraw=std::max(maxraw,d2);
  }
  printf("bandsPressure: |raster-floor(ref)| max=%.2f at(%d,%d)   |raster-ref| max=%.2f\n",maxfloor,fx,fy,maxraw);
  printf("ref uses 256*area; raster clamp at 255 hence full pixels 255 vs 256.\n");
  return 0;
}
