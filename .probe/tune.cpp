// Tune curve fixtures: report maxdiff (raster vs reference) for families of
// gentle conic/cubic material, minimized over subpixel phases.
#include <cstdio>
#include <vector>
#include <cmath>
#include <algorithm>
#include "raster_api.h"
#include "reference.h"
#include "fixtures.h"

static const FT_Raster_Funcs* api = &ft_grays_raster;

static double maxdiff(const FT_Outline&o,int W,int H){
  std::vector<unsigned char> g((size_t)W*H,0);
  FT_Bitmap bm{};bm.width=(unsigned)W;bm.rows=(unsigned)H;bm.pitch=W;
  bm.buffer=g.data();bm.num_grays=256;bm.pixel_mode=FT_PIXEL_MODE_GRAY;
  FT_Raster r;api->raster_new(nullptr,&r);
  FT_Raster_Params p{};p.source=&o;p.target=&bm;p.flags=FT_RASTER_FLAG_AA;
  api->raster_render(r,&p);api->raster_done(r);
  auto rr=ref::render(o,W,H,0);
  double mx=0;
  for(int y=0;y<H;++y)for(int x=0;x<W;++x)
    mx=std::max(mx,std::fabs((double)g[(size_t)(H-1-y)*W+x]-rr.gray[(size_t)y*W+x]));
  return mx;
}

// tall shape with a curved right side; right boundary bulges +b units at mid.
static fx::Shape tallSide(int Hpx,int b,int xoff,int yoff,int cubic){
  auto s=fx::Shape();
  int L=fx::px(1), R=fx::px(5);
  int H=Hpx*64;
  s.on(L+xoff,yoff).on(R+xoff,yoff);
  if(!cubic) s.conic(R+b+xoff,H/2+yoff);
  else { s.cubic(R+b+xoff,H/3+yoff); s.cubic(R+b+xoff,2*H/3+yoff); }
  s.on(R+xoff,H+yoff).on(L+xoff,H+yoff).end();
  return s;
}
// broad gentle arch, apex h units above base
static fx::Shape arch(int wpx,int h,int xoff,int yoff,int cubic){
  auto s=fx::Shape();
  int L=fx::px(1), W=wpx*64, base=fx::px(2);
  s.on(L+xoff,base+yoff);
  if(!cubic) s.conic(L+W/2+xoff,base+h+yoff);
  else { s.cubic(L+W/3+xoff,base+h+yoff); s.cubic(L+2*W/3+xoff,base+h+yoff); }
  s.on(L+W+xoff,base+yoff).on(L+W+xoff,base-fx::px(1)+yoff).on(L+xoff,base-fx::px(1)+yoff).end();
  return s;
}
// in-band y-extremum: rect-ish right side with conic apex; apex at base+h/2
static fx::Shape extremum(int h,int xoff,int yoff,int cubic){
  auto s=fx::Shape();
  int L=fx::px(1), R=fx::px(6), base=fx::px(2);
  s.on(L+xoff,base+yoff).on(R+xoff,base+yoff);
  if(!cubic) s.conic(R+fx::px(0.10)+xoff,base+h+yoff);
  else { s.cubic(R+fx::px(0.10)+xoff,base+h/3+yoff); s.cubic(R+fx::px(0.10)+xoff,base+2*h/3+yoff); }
  s.on(R+xoff,base+2*h+yoff).on(L+xoff,base+2*h+yoff).end();
  return s;
}

int main(){
  printf("TALL SIDE (H=100px, conic):\n");
  for(int b : {1,2,3,4,6,8,12,16,24}){
    double best=1e9;int bx=0,by=0;
    for(int xf=0;xf<8;++xf)for(int yf=0;yf<8;++yf){
      double d=maxdiff(tallSide(100,b,xf*8,yf*8,0).outline(),8,103);
      if(d<best){best=d;bx=xf;by=yf;}
    }
    printf("  b=%2d (%.3fpx) best=%.2f @(%d,%d)%s\n",b,b/64.0,best,bx,by,best<=3.5?" FIT":"");
  }
  printf("TALL SIDE (H=100px, cubic):\n");
  for(int b : {1,2,3,4,6,8,12,16,24}){
    double best=1e9;int bx=0,by=0;
    for(int xf=0;xf<8;++xf)for(int yf=0;yf<8;++yf){
      double d=maxdiff(tallSide(100,b,xf*8,yf*8,1).outline(),8,103);
      if(d<best){best=d;bx=xf;by=yf;}
    }
    printf("  b=%2d (%.3fpx) best=%.2f @(%d,%d)%s\n",b,b/64.0,best,bx,by,best<=3.5?" FIT":"");
  }
  printf("ARCH conic (w=4px):\n");
  for(int h : {8,16,24,32,40,48}){
    double best=1e9;int bx=0,by=0;
    for(int xf=0;xf<8;++xf)for(int yf=0;yf<8;++yf){
      double d=maxdiff(arch(4,h,xf*8,yf*8,0).outline(),8,8+ (h/64));
      if(d<best){best=d;bx=xf;by=yf;}
    }
    printf("  h=%2d (%.2fpx) best=%.2f @(%d,%d)%s\n",h,h/64.0,best,bx,by,best<=3.5?" FIT":"");
  }
  printf("ARCH cubic (w=4px):\n");
  for(int h : {8,16,24,32,40,48}){
    double best=1e9;int bx=0,by=0;
    for(int xf=0;xf<8;++xf)for(int yf=0;yf<8;++yf){
      double d=maxdiff(arch(4,h,xf*8,yf*8,1).outline(),8,8+(h/64));
      if(d<best){best=d;bx=xf;by=yf;}
    }
    printf("  h=%2d (%.2fpx) best=%.2f @(%d,%d)%s\n",h,h/64.0,best,bx,by,best<=3.5?" FIT":"");
  }
  printf("EXTREMUM conic (w=5px, apex h/2):\n");
  for(int h : {8,16,24,32,40,48}){
    double best=1e9;int bx=0,by=0;
    for(int xf=0;xf<8;++xf)for(int yf=0;yf<8;++yf){
      double d=maxdiff(extremum(h,xf*8,yf*8,0).outline(),9,9+(2*h)/64);
      if(d<best){best=d;bx=xf;by=yf;}
    }
    printf("  h=%2d (%.2fpx) best=%.2f @(%d,%d)%s\n",h,h/64.0,best,bx,by,best<=3.5?" FIT":"");
  }
  printf("EXTREMUM cubic (w=5px, apex h/2):\n");
  for(int h : {8,16,24,32,40,48}){
    double best=1e9;int bx=0,by=0;
    for(int xf=0;xf<8;++xf)for(int yf=0;yf<8;++yf){
      double d=maxdiff(extremum(h,xf*8,yf*8,1).outline(),9,9+(2*h)/64);
      if(d<best){best=d;bx=xf;by=yf;}
    }
    printf("  h=%2d (%.2fpx) best=%.2f @(%d,%d)%s\n",h,h/64.0,best,bx,by,best<=3.5?" FIT":"");
  }
  return 0;
}
