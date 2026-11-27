// Tune curve material: long wavy boundaries that span many rows, and short
// in-band x-extrema.  Objective: raster-vs-reference <= 4 gray (aim 3.5).
#include <cstdio>
#include <vector>
#include <cmath>
#include <algorithm>
#include "raster_api.h"
#include "reference.h"
#include "fixtures.h"

static const FT_Raster_Funcs* api = &ft_grays_raster;

static double maxdiff( const FT_Outline& o, int W, int H, double tol = 5e-4 )
{
  std::vector<unsigned char> g( (size_t)W*H, 0 );
  FT_Bitmap bm{}; bm.width=(unsigned)W; bm.rows=(unsigned)H; bm.pitch=W;
  bm.buffer=g.data(); bm.num_grays=256; bm.pixel_mode=FT_PIXEL_MODE_GRAY;
  FT_Raster r; api->raster_new(nullptr,&r);
  FT_Raster_Params p{}; p.source=&o; p.target=&bm; p.flags=FT_RASTER_FLAG_AA;
  api->raster_render(r,&p); api->raster_done(r);
  auto rr = ref::render(o,W,H,0,tol);
  double mx=0;
  for(int y=0;y<H;++y)for(int x=0;x<W;++x)
    mx=std::max(mx,std::fabs((double)g[(size_t)(H-1-y)*W+x]-rr.gray[(size_t)y*W+x]));
  return mx;
}

// Tall conic right boundary from (R,y0) to (R,y1); control (R+b, mid+d).
static fx::Shape tallConic( int Hpx, int b, int d, int xoff, int yoff )
{
  fx::Shape s;
  int L=fx::px(1), R=fx::px(5), H=Hpx*64;
  s.on(L+xoff,yoff).on(R+xoff,yoff)
   .conic(R+b+xoff,H/2+d+yoff)
   .on(R+xoff,H+yoff).on(L+xoff,H+yoff).end();
  return s;
}
// Tall cubic: controls near endpoints, both offset by b in x and d in y.
static fx::Shape tallCubic( int Hpx, int b, int d, int xoff, int yoff )
{
  fx::Shape s;
  int L=fx::px(1), R=fx::px(5), H=Hpx*64;
  s.on(L+xoff,yoff).on(R+xoff,yoff)
   .cubic(R+b+xoff,H/3+d+yoff)
   .cubic(R+b+xoff,2*H/3-d+yoff)
   .on(R+xoff,H+yoff).on(L+xoff,H+yoff).end();
  return s;
}
// In-band x-extremum: right boundary is a conic bulging +b over a short height.
static fx::Shape extConic( int vpx, int b, int xoff, int yoff )
{
  fx::Shape s;
  int L=fx::px(1), R=fx::px(5), V=vpx*64;
  s.on(L+xoff,yoff).on(R+xoff,yoff)
   .conic(R+b+xoff,V/2+yoff)
   .on(R+xoff,V+yoff)
   .on(L+xoff,V+yoff).end();
  return s;
}
static fx::Shape extCubic( int vpx, int b, int xoff, int yoff )
{
  fx::Shape s;
  int L=fx::px(1), R=fx::px(5), V=vpx*64;
  s.on(L+xoff,yoff).on(R+xoff,yoff)
   .cubic(R+b+xoff,V/3+yoff)
   .cubic(R+b+xoff,2*V/3+yoff)
   .on(R+xoff,V+yoff)
   .on(L+xoff,V+yoff).end();
  return s;
}

static void sweep4( const char* name, int W, int H,
                   fx::Shape (*gen)(int,int,int,int), int vpx, int b, int d )
{
  double best=1e9; int bx=0, by=0;
  for(int xf=0;xf<8;++xf)for(int yf=0;yf<8;++yf){
    fx::Shape s = gen(vpx,b,xf*8,yf*8);
    double m = maxdiff(s.outline(),W,H);
    if(m<best){best=m;bx=xf;by=yf;}
  }
  printf("%-34s vpx=%d b=%3d(%.2fpx) best=%5.2f @(%d,%d)%s\n",
         name,vpx,b,b/64.0,best,bx,by,best<=3.5?"  <== FIT":"");
}
static void sweep( const char* name, int W, int H,
                   fx::Shape (*gen)(int,int,int,int,int), int Hpx, int b, int d )
{
  double best=1e9; int bx=0, by=0;
  for(int xf=0;xf<8;++xf)for(int yf=0;yf<8;++yf){
    fx::Shape s = gen(Hpx,b,d,xf*8,yf*8);
    double m = maxdiff(s.outline(),W,H);
    if(m<best){best=m;bx=xf;by=yf;}
  }
  printf("%-34s Hpx=%d b=%3d(%.2fpx) d=%3d best=%5.2f @(%d,%d)%s\n",
         name,Hpx,b,b/64.0,d,best,bx,by,best<=3.5?"  <== FIT":"");
}

int main(){
  printf("--- tall conic spanning 160 rows (W=8,H=163) ---\n");
  for(int b : {1,2,4,8,12,16,24,32})
    for(int d : {0, 20*64/8, 20*64/4})
      sweep("tallConic",8,163,tallConic,160,b,d);
  printf("--- tall cubic spanning 160 rows ---\n");
  for(int b : {1,2,4,8,12,16,24,32})
    for(int d : {0, 20*64/8, 20*64/4})
      sweep("tallCubic",8,163,tallCubic,160,b,d);
  printf("--- in-band conic extremum (V=16 units) ---\n");
  for(int vpx : {16,24,32}) for(int b : {8,16,24,32,40})
    sweep4("extConic",8,vpx+3,extConic,vpx,b,0);
  printf("--- in-band cubic extremum ---\n");
  for(int vpx : {16,24,32}) for(int b : {8,16,24,32,40})
    sweep4("extCubic",8,vpx+3,extCubic,vpx,b,0);
  return 0;
}
