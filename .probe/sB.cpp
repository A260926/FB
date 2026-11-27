// Fast joint search with coarse-to-fine strategy.  Stage 1 scans curvature and
// control position at 1/4 px over a small set; stage 2 refines the single best
// candidate at 1/16 px.  Prints concrete integer geometry.
#include <cstdio>
#include <vector>
#include <cmath>
#include <algorithm>
#include "raster_api.h"
#include "reference.h"
#include "fixtures.h"

static double md( const FT_Outline& o, int W,int H,int fl=0){
  std::vector<unsigned char> g((size_t)W*H,0);
  FT_Bitmap bm{};bm.width=W;bm.rows=H;bm.pitch=W;bm.buffer=g.data();bm.num_grays=256;bm.pixel_mode=FT_PIXEL_MODE_GRAY;
  FT_Raster r;ft_grays_raster.raster_new(nullptr,&r);FT_Raster_Params p{};p.source=&o;p.target=&bm;p.flags=FT_RASTER_FLAG_AA;
  ft_grays_raster.raster_render(r,&p);ft_grays_raster.raster_done(r);
  auto rr=ref::render(o,W,H,fl,1.5e-4);double mx=0;
  for(int y=0;y<H;++y)for(int x=0;x<W;++x)mx=std::max(mx,std::fabs((double)g[(size_t)(H-1-y)*W+x]-rr.gray[(size_t)y*W+x]));
  return mx;
}

int main(){
  int Hpx=80,H=Hpx*64,L=fx::px(1),R=fx::px(5);
  // Stage 1: vary b and control y fraction, phase 1/4 px
  struct C{int b,fr;}; double bestE=1e9; int bestB=0,bestFr=0,bestX=0,bestY=0;
  for(int b=8;b<=48;b+=8) for(int fr=2;fr<=14;fr+=2) {
    for(int xf=0;xf<8;++xf) for(int yf=0;yf<8;++yf) {
      int cy=H*fr/16;
      fx::Shape s; s.on(L+xf*8,yf*8).on(R+xf*8,yf*8).conic(R+b+xf*8,cy+yf*8).on(R+xf*8,H+yf*8).on(L+xf*8,H+yf*8).end();
      double e=md(s.outline(),8,Hpx+3);
      if(e<bestE){bestE=e;bestB=b;bestFr=fr;bestX=xf*8;bestY=yf*8;}
    }
  }
  printf("stage1 tallConic: err=%.2f b=%d fr=%d x=%d y=%d\n",bestE,bestB,bestFr,bestX,bestY);
  // Stage 2 refine around best at 1/16 px (2 subpixel steps)
  double e2=1e9; int ex=0,ey=0;
  for(int dx=-8;dx<=8;++dx)for(int dy=-8;dy<=8;++dy){
    int cy=H*bestFr/16;
    fx::Shape s; s.on(L+bestX+dx,8+dy).on(R+bestX+dx,8+dy).conic(R+bestB+bestX+dx,cy+bestY+dy).on(R+bestX+dx,H+8+dy).on(L+bestX+dx,H+8+dy).end();
    double e=md(s.outline(),8,Hpx+3); if(e<e2){e2=e;ex=dx;ey=dy;}
  }
  printf("stage2 tallConic: err=%.2f xoff=%d yoff=%d\n",e2,bestX+ex,bestY+ey);
  return 0;
}
