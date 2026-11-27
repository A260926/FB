// Validate reference against exact polygon clipping of a densely flattened
// curve, independent of rasterizer.
#include <cstdio>
#include <vector>
#include <cmath>
#include <algorithm>
#include "raster_api.h"
#include "reference.h"
#include "fixtures.h"
struct P{double x,y;};
static std::vector<P> clip(std::vector<P> p,double x0,double y0,double x1,double y1){
  auto c1=[&](std::vector<P> in,int ax,double v,bool gt){std::vector<P>o;if(in.empty())return o;
    for(size_t i=0;i<in.size();++i){P a=in[i],b=in[(i+1)%in.size()];double va=ax?a.y:a.x,vb=ax?b.y:b.x;bool ia=gt?va>=v:va<=v,ib=gt?vb>=v:vb<=v;if(ia)o.push_back(a);if(ia!=ib){double t=(v-va)/(vb-va);o.push_back({a.x+t*(b.x-a.x),a.y+t*(b.y-a.y)});}}return o;};
  p=c1(p,0,x0,true);p=c1(p,0,x1,false);p=c1(p,1,y0,true);p=c1(p,1,y1,false);return p;}
static double area(const std::vector<P>&p){double a=0;for(size_t i=0;i<p.size();++i){auto u=p[i],v=p[(i+1)%p.size()];a+=u.x*v.y-v.x*u.y;}return std::fabs(a)/2;}
// dense flatten of one cubic into polygon (closed via given closing lines)
static std::vector<P> denseCubic(double x0,double y0,double c0x,double c0y,double c1x,double c1y,double x1,double y1,int N=4096){
  std::vector<P> q; q.push_back({x0,y0});
  for(int i=1;i<=N;++i){double t=(double)i/N,u=1-t;
    q.push_back({u*u*u*x0+3*u*u*t*c0x+3*u*t*t*c1x+t*t*t*x1,
                 u*u*u*y0+3*u*u*t*c0y+3*u*t*t*c1y+t*t*t*y1});}
  return q;
}
int main(){
  // quarter-circle cubic R=3 device px, from (1,1) to (4,4), controls (4,1+3-? ) standard: c1=(1+3,1+3*(1-k/R))...
  double R=3, k=0.55228475*R;
  std::vector<P> poly{{1,1},{1+R,1+R-k},{1+R-k,1+R},{1+R,1+R},{1+R,1}}; // close: (4,4)->(4,1)->(1,1)
  // replace curve part with dense
  std::vector<P> base=denseCubic(1,1,1+R,1+R-k,1+R-k,1+R,1+R,1+R);
  base.push_back({1+R,1}); // then close to start (1,1)
  fx::Shape s;
  s.on(fx::px(1),fx::px(1)).cubic(fx::px(1+R),fx::px(1+R-k)).cubic(fx::px(1+R-k),fx::px(1+R)).on(fx::px(1+R),fx::px(1+R)).on(fx::px(1+R),fx::px(1)).end();
  auto o=s.outline();
  int W=7,H=6; auto rr=ref::render(o,W,H,0,1e-6);
  double mx=0;int ax=0,ay=0; double ma=0;
  for(int y=0;y<H;++y)for(int x=0;x<W;++x){
    auto q=clip(base,x,y,x+1,y+1); double A=area(q)*256.0;
    double d=std::fabs(rr.rawGray[(size_t)y*W+x]-A);
    if(d>mx){mx=d;ax=x;ay=y;}
    ma=std::max(ma,A);
  }
  printf("ref vs exact-clip: max|raw256 - A256| = %.3f at(%d,%d)\n",mx,ax,ay);
  // raster vs exact clip
  std::vector<unsigned char> g((size_t)W*H,0);
  FT_Bitmap bm{};bm.width=W;bm.rows=H;bm.pitch=W;bm.buffer=g.data();bm.num_grays=256;bm.pixel_mode=FT_PIXEL_MODE_GRAY;
  FT_Raster r;ft_grays_raster.raster_new(nullptr,&r);FT_Raster_Params p{};p.source=&o;p.target=&bm;p.flags=FT_RASTER_FLAG_AA;
  ft_grays_raster.raster_render(r,&p);ft_grays_raster.raster_done(r);
  double mr=0;int rx=0,ry=0;
  for(int y=0;y<H;++y)for(int x=0;x<W;++x){auto q=clip(base,x,y,x+1,y+1);double A=area(q)*256.0;int gg=g[(size_t)(H-1-y)*W+x];double d=std::fabs(gg-A);if(d>mr){mr=d;rx=x;ry=y;}}
  printf("raster vs exact-clip: max = %.2f at(%d,%d)  (true curve area)\n",mr,rx,ry);
  return 0;
}
