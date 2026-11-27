// Survey curve fixtures for the 4-gray fixed-curve tolerance.
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
  for(int y=0;y<H;++y)for(int x=0;x<W;++x){
    double d=std::fabs(g[(size_t)(H-1-y)*W+x]-r[(size_t)y*W+x]);mx=std::max(mx,d);}
  return mx;
}

struct B {
  std::vector<FT_Vector> p; std::vector<char> t; short c;
  void add(FT_Vector v,char tag){p.push_back(v);t.push_back(tag);}
  FT_Outline out(){c=(short)(p.size()-1);FT_Outline o{};o.n_contours=1;o.n_points=(short)p.size();o.points=p.data();o.tags=t.data();o.contours=&c;return o;}
};

// upper arch: start (x0,yb), curve to (x1,yb) via controls, then nothing (closed by line back)
static B archCubic(int x0,int yb,int c1x,int c1y,int c2x,int c2y,int x1,int yb2){
  B b; b.add({x0,yb},1); b.add({c1x,c1y},2); b.add({c2x,c2y},2); b.add({x1,yb2},1); return b;
}
static B archConic(int x0,int yb,int cx,int cy,int x1,int yb2){
  B b; b.add({x0,yb},1); b.add({cx,cy},0); b.add({x1,yb2},1); return b;
}

int main(){
  printf("--- half-circle-ish arcs (device) ---\n");
  for(double R : {1.0,1.5,2.0,3.0,4.0,6.0,8.0}){
    int R6=(int)llround(R*64), k6=(int)llround(0.55228475*R*64);
    B b; b.add({64,64},1);
    b.add({64+R6,64+k6},2);
    b.add({64+k6,64+R6},2);
    b.add({64,64+R6},1);
    int W=(int)std::ceil(1+R)+2, H=(int)std::ceil(1+R)+2;
    char nm[64]; std::snprintf(nm,sizeof nm,"quarter cubic R=%.1f",R);
    printf("%-40s maxdiff=%.2f\n",nm,maxdiff(b.out(),W,H));
  }
  printf("--- conic arches (apex height h px over span w px) ---\n");
  int spans[]={128,192,256,384,512};
  int heights[]={16,32,64,96,128};
  for(int si=0;si<5;++si)for(int hi=0;hi<5;++hi){
    int w=spans[si], h=heights[hi];
    B b=archConic(64,64,64+w/2,64+h,64+w,64);
    int W=w/64+3,H=h/64+3;
    char nm[64]; std::snprintf(nm,sizeof nm,"conic w=%d h=%d (px %.0f,%.0f)",w,h,w/64.0,h/64.0);
    printf("%-40s maxdiff=%.2f\n",nm,maxdiff(b.out(),W,H));
  }
  printf("--- cubic arches ---\n");
  for(int si=0;si<5;++si)for(int hi=0;hi<5;++hi){
    int w=spans[si], h=heights[hi];
    B b=archCubic(64,64,64+w/3,64+h,64+2*w/3,64+h,64+w,64);
    int W=w/64+3,H=h/64+3;
    char nm[64]; std::snprintf(nm,sizeof nm,"cubic w=%d h=%d (px %.0f,%.0f)",w,h,w/64.0,h/64.0);
    printf("%-40s maxdiff=%.2f\n",nm,maxdiff(b.out(),W,H));
  }
  return 0;
}
