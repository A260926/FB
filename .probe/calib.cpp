// Scratch calibration: compare rasterizer against the independent reference
// on a few simple shapes, before wiring the real test file.
#include <cstdio>
#include <cstring>
#include <vector>
#include <cmath>
#include "raster_api.h"
#include "reference.h"

static void pixmap( const FT_Outline& o, int W, int H, std::vector<unsigned char>& out )
{
  out.assign( (size_t)W * H, 0 );
  FT_Bitmap bm{}; bm.width=(unsigned)W; bm.rows=(unsigned)H; bm.pitch=W;
  bm.buffer=out.data(); bm.num_grays=256; bm.pixel_mode=FT_PIXEL_MODE_GRAY;
  FT_Raster r; ft_grays_raster.raster_new(nullptr,&r);
  FT_Raster_Params p{}; p.source=&o; p.target=&bm; p.flags=FT_RASTER_FLAG_AA;
  ft_grays_raster.raster_render(r,&p);
  ft_grays_raster.raster_done(r);
}

static void show( const char* name, const FT_Outline& o, int W, int H )
{
  std::vector<unsigned char> got; pixmap(o,W,H,got);
  std::vector<double> refc; double me=0,ae=0;
  ref::render(o,W,H,0,refc,me,ae);
  int maxdiff=0;
  for(int y=0;y<H;++y)for(int x=0;x<W;++x){
    int a=got[(size_t)(H-1-y)*W+x];
    double b=refc[(size_t)y*W+x];
    int d=(int)std::lround(std::fabs(a-b));
    if(d>maxdiff) maxdiff=d;
  }
  printf("%s maxErr=%g areaErr=%g maxdiff=%d\n",name,me,ae,maxdiff);
  for(int y=0;y<H;++y){
    for(int x=0;x<W;++x){
      int a=got[(size_t)(H-1-y)*W+x]; double b=refc[(size_t)y*W+x];
      printf("%4d/%5.1f ",a,b);
    }
    printf("\n");
  }
}

int main()
{
  FT_Vector pts[] = {{64,64},{192,64},{192,192},{64,192}};
  char tags[]={1,1,1,1}; short ct[]={3};
  FT_Outline o{}; o.n_contours=1;o.n_points=4;o.points=pts;o.tags=tags;o.contours=ct;
  show("rect",o,5,5);

  FT_Vector p2[] = {{64,64},{192,64},{256,128},{192,192},{64,192}};
  char t2[]={1,1,0,1,1}; short c2[]={4};
  FT_Outline o2{}; o2.n_contours=1;o2.n_points=5;o2.points=p2;o2.tags=t2;o2.contours=c2;
  show("conic",o2,6,6);
  return 0;
}
