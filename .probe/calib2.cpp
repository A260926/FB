// Scratch calibration for slanted lines and various curves.
#include <cstdio>
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

static void cmp( const char* name, const FT_Outline& o, int W, int H, int flags=0 )
{
  std::vector<unsigned char> got; pixmap(o,W,H,got);
  std::vector<double> refc; double me=0,ae=0;
  ref::render(o,W,H,flags,refc,me,ae);
  double maxdiff=0; int atx=0,aty=0; int gv=0; double rv=0;
  for(int y=0;y<H;++y)for(int x=0;x<W;++x){
    int a=got[(size_t)(H-1-y)*W+x];
    double b=refc[(size_t)y*W+x];
    double d=std::fabs(a-b);
    if(d>maxdiff){maxdiff=d;atx=x;aty=y;gv=a;rv=b;}
  }
  printf("%-28s maxErr=%.3g areaErr=%.3g maxdiff=%.2f at(%d,%d) got=%d ref=%.1f\n",
         name,me,ae,maxdiff,atx,aty,gv,rv);
}

int main()
{
  // slanted triangle: (1,1)->(5,1)->(1,4)
  FT_Vector t1[] = {{64,64},{320,64},{64,256}};
  char tg1[]={1,1,1}; short c1[]={2};
  FT_Outline o1{}; o1.n_contours=1;o1.n_points=3;o1.points=t1;o1.tags=tg1;o1.contours=c1;
  cmp("triangle",o1,7,6);

  // slanted quad non-integer
  FT_Vector t2[] = {{80,48},{320,96},{288,288},{48,240}};
  char tg2[]={1,1,1,1}; short c2[]={3};
  FT_Outline o2{}; o2.n_contours=1;o2.n_points=4;o2.points=t2;o2.tags=tg2;o2.contours=c2;
  cmp("slanted quad",o2,7,6);

  // conic bulging right, span 2px (previous)
  FT_Vector p3[] = {{64,64},{192,64},{256,128},{192,192},{64,192}};
  char t3[]={1,1,0,1,1}; short c3[]={4};
  FT_Outline o3{}; o3.n_contours=1;o3.n_points=5;o3.points=p3;o3.tags=t3;o3.contours=c3;
  cmp("conic bulge",o3,6,6);

  // conic small bulge: control (3,1)->(3.75,2)->(3,3)
  FT_Vector p4[] = {{64,64},{192,64},{240,128},{192,192},{64,192}};
  char t4[]={1,1,0,1,1}; short c4[]={4};
  FT_Outline o4{}; o4.n_contours=1;o4.n_points=5;o4.points=p4;o4.tags=t4;o4.contours=c4;
  cmp("conic small",o4,6,6);

  // cubic bulge: (3,1) c(4,1.5) c(4,2.5) (3,3)
  FT_Vector p5[] = {{64,64},{192,64},{256,96},{256,160},{192,192},{64,192}};
  char t5[]={1,1,2,2,1,1}; short c5[]={5};
  FT_Outline o5{}; o5.n_contours=1;o5.n_points=6;o5.points=p5;o5.tags=t5;o5.contours=c5;
  cmp("cubic bulge",o5,6,6);

  // cubic gentle
  FT_Vector p6[] = {{64,64},{192,64},{224,96},{224,160},{192,192},{64,192}};
  char t6[]={1,1,2,2,1,1}; short c6[]={5};
  FT_Outline o6{}; o6.n_contours=1;o6.n_points=6;o6.points=p6;o6.tags=t6;o6.contours=c6;
  cmp("cubic gentle",o6,6,6);
  return 0;
}
