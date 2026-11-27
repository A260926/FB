// Isolate FreeType's own curve approximation error: render a curve clue and
// the same curve as a dense polyline through the same rasterizer.
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

static void cmp( const char* tag, const std::vector<unsigned char>& a,
                 const std::vector<unsigned char>& b, int W, int H )
{
  double mx=0; int ax=0,ay=0;
  for(int y=0;y<H;++y)for(int x=0;x<W;++x){
    double d=std::fabs((double)a[(size_t)(H-1-y)*W+x]-b[(size_t)(H-1-y)*W+x]);
    if(d>mx){mx=d;ax=x;ay=y;}
  }
  printf("%-30s maxdiff=%.2f at(%d,%d)\n",tag,mx,ax,ay);
}

// dense polyline of a cubic into outline points, integer 26.6
static void cubicPoly( std::vector<FT_Vector>& pts, std::vector<char>& tags, short& ct,
                       double x0,double y0,double c0x,double c0y,double c1x,double c1y,double x1,double y1, int N )
{
  pts.clear(); tags.clear();
  for(int i=1;i<=N;++i){
    double t=(double)i/N; double u=1-t;
    double X=u*u*u*x0+3*u*u*t*c0x+3*u*t*t*c1x+t*t*t*x1;
    double Y=u*u*u*y0+3*u*u*t*c0y+3*u*t*t*c1y+t*t*t*y1;
    pts.push_back({(FT_Pos)llround(X*64),(FT_Pos)llround(Y*64)});
    tags.push_back(1);
  }
  ct=(short)(pts.size()-1);
}

int main()
{
  int W=8,H=8;
  // gentle cubic from (1,1) to (1,3)? use (1,1)->(4,1)->controls (5,2),(5,2)->(4,4)->(1,4)
  // build curve outline: start on (1,1)
  std::vector<FT_Vector> cp;
  cp.push_back({64,64});
  cp.push_back({256,192});  // control1  (4,3)
  cp.push_back({256,320});  // control2  (4,5)
  cp.push_back({384,384});  // end (6,6)
  std::vector<char> ctg={1,2,2,1};
  short cc=3;
  // close back with lines
  cp.push_back({384,64}); // (6,1)
  cp.push_back({64,64});
  ctg.push_back(1); ctg.push_back(1);
  // need contour includes all: rebuild properly
  std::vector<FT_Vector> P;
  P.push_back({64,64});    // start (1,1)
  P.push_back({256,192});  // c1 (4,3)
  P.push_back({256,320});  // c2 (4,5)
  P.push_back({384,384});  // end (6,6)
  P.push_back({384,64});   // (6,1)
  std::vector<char> T={1,2,2,1,1};
  short C=4;
  FT_Outline oc{}; oc.n_contours=1;oc.n_points=(short)P.size();oc.points=P.data();oc.tags=T.data();oc.contours=&C;

  // dense version: same curve as polyline from (1,1) with cubic, then lines
  std::vector<FT_Vector> DP;
  std::vector<char> DT;
  DP.push_back({64,64}); DT.push_back(1);
  {
    double x0=1,y0=1,c0x=4,c0y=3,c1x=4,c1y=5,x1=6,y1=6;
    int N=96;
    for(int i=1;i<=N;++i){double t=(double)i/N,u=1-t;
      double X=u*u*u*x0+3*u*u*t*c0x+3*u*t*t*c1x+t*t*t*x1;
      double Y=u*u*u*y0+3*u*u*t*c0y+3*u*t*t*c1y+t*t*t*y1;
      DP.push_back({(FT_Pos)llround(X*64),(FT_Pos)llround(Y*64)}); DT.push_back(1);}
  }
  DP.push_back({384,64}); DT.push_back(1);
  short DC=(short)(DP.size()-1);
  FT_Outline od{}; od.n_contours=1;od.n_points=(short)DP.size();od.points=DP.data();od.tags=DT.data();od.contours=&DC;

  std::vector<unsigned char> a,b; pixmap(oc,W,H,a); pixmap(od,W,H,b);
  cmp("cubic vs dense-poly (raster)",a,b,W,H);

  std::vector<double> ra,rb; double e1,e2,ae1,ae2;
  ref::render(oc,W,H,0,ra,e1,ae1);
  ref::render(od,W,H,0,rb,e2,ae2);
  double mx=0;int ax=0,ay=0;
  for(int y=0;y<H;++y)for(int x=0;x<W;++x){double d=std::fabs(ra[(size_t)y*W+x]-rb[(size_t)y*W+x]);if(d>mx){mx=d;ax=x;ay=y;}}
  printf("%-30s maxdiff=%.2f at(%d,%d) refErr=%g\n","cubic vs dense-poly (ref)",mx,ax,ay,e1);

  // raster(dense) vs ref(dense) and raster(cubic) vs ref(cubic)
  double m1=0;for(int y=0;y<H;++y)for(int x=0;x<W;++x)m1=std::max(m1,std::fabs((double)b[(size_t)(H-1-y)*W+x]-rb[(size_t)y*W+x]));
  printf("dense raster vs ref: %.2f\n",m1);
  double m2=0;for(int y=0;y<H;++y)for(int x=0;x<W;++x)m2=std::max(m2,std::fabs((double)a[(size_t)(H-1-y)*W+x]-ra[(size_t)y*W+x]));
  printf("cubic raster vs ref: %.2f\n",m2);
  return 0;
}
