// Scratch: measure every fixture against the independent reference and both
// rasterizer pools.  Used only to pick tolerances; removed before finishing.
#include <cstdio>
#include <vector>
#include <cmath>
#include <algorithm>
#include "raster_api.h"
#include "reference.h"
#include "fixtures.h"

static const FT_Raster_Funcs* apis[2] = { &ft_grays_raster, &ft_grays_raster_small };
static const char* names[2] = { "16K", "2K" };

static void renderRaster( const FT_Raster_Funcs& api, const FT_Outline& o,
                          int W, int H, std::vector<unsigned char>& out )
{
  out.assign( (size_t)W * H, 0 );
  FT_Bitmap bm{}; bm.width=(unsigned)W; bm.rows=(unsigned)H; bm.pitch=W;
  bm.buffer=out.data(); bm.num_grays=256; bm.pixel_mode=FT_PIXEL_MODE_GRAY;
  FT_Raster r; api.raster_new( nullptr, &r );
  FT_Raster_Params p{}; p.source=&o; p.target=&bm; p.flags=FT_RASTER_FLAG_AA;
  api.raster_render( r, &p ); api.raster_done( r );
}

static void report( const char* name, const fx::Shape& s, int W, int H,
                   int fillFlags = 0 )
{
  FT_Outline o = s.outline( fillFlags );
  auto rr = ref::render( o, W, H, fillFlags );
  printf("%-28s ref maxErr=%.3g areaErr=%.3g", name, rr.maxErr, rr.areaErr);
  for ( int a = 0; a < 2; ++a ) {
    std::vector<unsigned char> got;
    renderRaster( *apis[a], o, W, H, got );
    double mx = 0; int bx=0,by=0,bg=0; double br=0;
    for ( int y=0;y<H;++y) for ( int x=0;x<W;++x) {
      int g = got[(size_t)(H-1-y)*W+x];
      double rv = rr.gray[(size_t)y*W+x];
      double d = std::fabs( g - rv );
      if ( d > mx ) { mx=d; bx=x; by=y; bg=g; br=rv; }
    }
    printf("  %s %.2f@(%d,%d)g%d/r%.1f", names[a], mx, bx, by, bg, br);
  }
  printf("\n");
}

int main(){
  report("bandsPressure", fx::bandsPressure(), 100, 166, 0);
  report("bandsCurves", fx::bandsCurves(), 70, 104, 0);
  report("impliedConicStartOnEnd", fx::impliedConicStartOnEnd(), 8, 6);
  report("impliedConicStartOffEnd", fx::impliedConicStartOffEnd(), 8, 6);
  report("tallConic", fx::tallConic(), 8, 102);
  report("tallCubic", fx::tallCubic(), 8, 102);
  report("conicExtremum", fx::conicExtremum(), 9, 9);
  report("cubicExtremum", fx::cubicExtremum(), 9, 9);
  report("innerLoop same", fx::innerLoop(false), 14, 14);
  report("innerLoop reverse", fx::innerLoop(true), 14, 14);
  report("innerLoop same EO", fx::innerLoop(false), 14, 14, ref::evenOddFlag());
  report("innerLoop reverse EO", fx::innerLoop(true), 14, 14, ref::evenOddFlag());
  report("ring", fx::ring(), 15, 12);
  report("ring EO", fx::ring(), 15, 12, ref::evenOddFlag());
  report("touchingEdge", fx::touchingEdge(), 12, 9);
  report("touchingCorner", fx::touchingCorner(), 12, 12);
  report("duplicateRect NZ", fx::duplicateRect(), 12, 12);
  report("duplicateRect EO", fx::duplicateRect(), 12, 12, ref::evenOddFlag());
  report("layoutMixed", fx::layoutMixed(), 18, 12);
  report("negativeShape", fx::negativeShape(), 10, 8);
  return 0;
}
