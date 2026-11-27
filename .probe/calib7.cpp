// Survey curve cases: rasterizer vs independent reference max gray diff.
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

struct Res{ double maxd; int x,y; int got; double refv; };
static Res compare(const FT_Outline&o,int W,int H,int flags=0){
  std::vector<unsigned char>g;pixmap(o,W,H,g);
  std::vector<double>r;double me,ae;ref::render(o,W,H,flags,r,me,ae);
  Res z{0,0,0,0,0};
  for(int y=0;y<H;++y)for(int x=0;x<W;++x){
    int a=g[(size_t)(H-1-y)*W+x]; double b=r[(size_t)y*W+x];
    double d=std::fabs(a-b); if(d>z.maxd){z.maxd=d;z.x=x;z.y=y;z.got=a;z.refv=b;}
  }
  return z;
}

// Build a closed outline: start (x0,y0), conic control (cx,cy) to (x1,y1), then
// straight rectangle-ish return path along the bottom.
static FT_Outline conicBulge(int x0,int y0,int cx,int cy,int x1,int y1,int bottom)
{
  static std::vector<FT_Vector> pts; static std::vector<char> tags; static std::vector<short> ct; // not thread safe; scratch
  pts.clear();tags.clear();ct.clear();
  pts.push_back({x0,y0}); tags.push_back(1);
  pts.push_back({cx,cy}); tags.push_back(0);
  pts.push_back({x1,y1}); tags.push_back(1);
  pts.push_back({x1,bottom}); tags.push_back(1);
  pts.push_back({x0,bottom}); tags.push_back(1);
  ct.push_back((short)(pts.size()-1));
  FT_Outline o{};o.n_contours=1;o.n_points=(short)pts.size();o.points=pts.data();o.tags=tags.data();o.contours=ct.data();
  return o;
}

static FT_Outline cubicBulge(int x0,int y0,int c1x,int c1y,int c2x,int c2y,int x1,int y1,int bottom)
{
  static std::vector<FT_Vector> pts; static std::vector<char> tags; static std::vector<short> ct;
  pts.clear();tags.clear();ct.clear();
  pts.push_back({x0,y0}); tags.push_back(1);
  pts.push_back({c1x,c1y}); tags.push_back(2);
  pts.push_back({c2x,c2y}); tags.push_back(2);
  pts.push_back({x1,y1}); tags.push_back(1);
  pts.push_back({x1,bottom}); tags.push_back(1);
  pts.push_back({x0,bottom}); tags.push_back(1);
  ct.push_back((short)(pts.size()-1));
  FT_Outline o{};o.n_contours=1;o.n_points=(short)pts.size();o.points=pts.data();o.tags=tags.data();o.contours=ct.data();
  return o;
}

int main(){
  printf("=== conic bulges: base y=1..3 (64..192), x from 1 to 6 (64..384), bottom=1 ===\n");
  struct C{int cx,cy,name;};
  int cxs[]={0,8,16,24,32,40,48,56,64};
  for(int k=0;k<9;++k){
    int bulge=cxs[k];
    auto o=conicBulge(64,64,384+bulge,128,384,192,64);
    Res r=compare(o,8,5);
    printf("conic bulge=%2d/64px maxdiff=%.2f at(%d,%d) got=%d ref=%.1f\n",bulge,r.maxd,r.x,r.y,r.got,r.refv);
  }
  printf("=== cubic bulges ===\n");
  for(int k=0;k<9;++k){
    int bulge=cxs[k];
    auto o=cubicBulge(64,64,384+bulge,96,384+bulge,160,384,192,64);
    Res r=compare(o,8,5);
    printf("cubic bulge=%2d/64px maxdiff=%.2f at(%d,%d) got=%d ref=%.1f\n",bulge,r.maxd,r.x,r.y,r.got,r.refv);
  }
  return 0;
}
