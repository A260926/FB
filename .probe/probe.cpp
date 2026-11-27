// Probe: render a tall outline with many narrow stripes and print the
// real band/bisect trace.  Untracked scratch, deleted after calibration.
#include <cstdio>
#include <cstring>
#include <vector>
#include <cmath>
#include "raster_api.h"

static void build( std::vector<FT_Vector>& pts, std::vector<char>& tags,
                   std::vector<short>& contours, int stripes, int first_x,
                   int top_y, int bottom_y )
{
  pts.clear(); tags.clear(); contours.clear();
  for ( int i = 0; i < stripes; ++i )
  {
    int x0 = first_x + i * 9;
    int x1 = x0 + 3;
    pts.push_back( { x0*64, bottom_y*64 } );
    pts.push_back( { x1*64, bottom_y*64 } );
    pts.push_back( { x1*64, top_y*64 } );
    pts.push_back( { x0*64, top_y*64 } );
    tags.push_back( 1 ); tags.push_back( 1 );
    tags.push_back( 1 ); tags.push_back( 1 );
    contours.push_back( (short)( (int)pts.size() - 1 ) );
  }
}

int main( int argc, char** argv )
{
  int stripes = argc > 1 ? atoi( argv[1] ) : 20;
  int top = argc > 2 ? atoi( argv[2] ) : 120;
  int bot = argc > 3 ? atoi( argv[3] ) : 0;
  std::vector<FT_Vector> pts; std::vector<char> tags; std::vector<short> contours;
  build( pts, tags, contours, stripes, 1, top, bot );

  FT_Outline outline{};
  outline.n_contours = (short)contours.size();
  outline.n_points   = (short)pts.size();
  outline.points     = pts.data();
  outline.tags       = tags.data();
  outline.contours   = contours.data();
  outline.flags      = FT_OUTLINE_NONE;

  int W = 1 + stripes * 9 + 4;
  int H = top - bot + 4;
  std::vector<unsigned char> pix( (size_t)W * H, 0 );
  FT_Bitmap bm{}; bm.width=(unsigned)W; bm.rows=(unsigned)H; bm.pitch=W;
  bm.buffer=pix.data(); bm.num_grays=256; bm.pixel_mode=FT_PIXEL_MODE_GRAY;

  FT_Raster raster; if ( ft_grays_raster.raster_new( nullptr, &raster ) ) return 1;
  FT_Raster_Params p{}; p.source=&outline; p.target=&bm; p.flags=FT_RASTER_FLAG_AA;
  int err = ft_grays_raster.raster_render( raster, &p );
  ft_grays_raster.raster_done( raster );
  long covered = 0;
  for ( auto v : pix ) if ( v ) covered++;
  std::fprintf( stderr, "PROBE stripes=%d area=%d..%d err=%d nonzero_px=%ld\n",
                stripes, bot, top, err, covered );
  return err ? 2 : 0;
}
