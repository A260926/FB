// Sweep curve control deviation to find fixtures whose rasterizer-vs-reference
// max gray error stays <= 4.
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
static double maxdiff(const FT_Outline&o,int W,int H,int flags=0){
  std::vector<unsigned char>g;pixmap(o,W,H,g);
  std::vector<double>r;double me,ae;ref::render(o,W,H,flags,r,me,ae);
  double mx=0;
  for(int y=0;y<H;++y)for(int x=0;x<W;++x)
    mx=std::max(mx,std::fabs((double)g[(size_t)(H-1-y)*W+x]-r[(size_t)y*W+x]));
  return mx;
}
struct B{std::vector<FT_Vector>p;std::vector<char>t;short c;
  void add(FT_Vector v,char g){p.push_back(v);t.push_back(g);}
  FT_Outline out(){c=(short)(p.size()-1);FT_Outline o{};o.n_contours=1;o.n_points=(short)p.size();o.points=p.data();o.tags=t.data();o.contours=&c;return o;}
};

int main(){
  // conic "bump" on the right side of a vertical band: contour is a tall thin
  // shape whose right boundary bulges by b subpixels over height h.
  // start (1,1) -> conic ctrl (1+b, mid) -> (1, h+1) -> line back left.
  printf("conic right-boundary bulge (unit px, b subpix):\n");
  for(int hpx : {6,10,16,24}) for(int bsub : {0,4,8,12,16,24,32}) {
    if(bsub==0) continue;
    int h=hpx*64, b=bsub;
    int W=(int)std::ceil(hpx?1+1.0:2)+2;
    B bb; bb.add({64,64},1); bb.add({64+b,64+h/2},0); bb.add({64,64+h},1); bb.add({64,64+h},1);
    // close: line back to start automatically (last point = start? we duplicated) — build clean
    bb.p.clear();bb.t.clear();
    bb.add({64,64},1); bb.add({64+b,64+h/2},0); bb.add({64,64+h},1);
    double bpx=bsub/64.0, hpxd=hpx;
    char nm[80]; std::snprintf(nm,sizeof nm,"conic h=%dpx b=%.2fpx",hpx,bpx);
    printf("%-34s maxdiff=%.2f (D=%d subpix)\n",nm,maxdiff(bb.out(),4,hpx+3), bsub);
  }
  printf("\ncubic right-boundary bulge:\n");
  for(int hpx : {6,10,16,24}) for(int bsub : {4,8,12,16,24,32,48}) {
    int h=hpx*64, b=bsub;
    B bb; bb.add({64,64},1); bb.add({64+b,64+h/3},2); bb.add({64+b,64+2*h/3},2); bb.add({64,64+h},1);
    char nm[80]; std::snprintf(nm,sizeof nm,"cubic h=%dpx b=%.2fpx",hpx,bsub/64.0);
    printf("%-34s maxdiff=%.2f\n",nm,maxdiff(bb.out(),4,hpx+3));
  }
  return 0;
}
