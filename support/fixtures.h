// Test material for the gray-rasterizer regression suite.
//
// All coordinates are integer 26.6 fixed point (one device pixel == 64 units).
// Shapes are assembled from raw FT_Outline points, tags and contours; nothing
// here calls into FreeType.
//
// Every curved fixture was chosen and phase-tuned so that FreeType's own
// chord approximation stays inside the 4-gray fixed-curve budget on both
// rasterizer builds (measured; see README).  Straight fixtures are exact under
// the reference's floor model and only differ by the rasterizer's own 1-gray
// quantization.

#pragma once

#include <cstddef>
#include <vector>

#include "raster_api.h"

namespace fx {

inline int px( double v ) {
  return (int)( v * 64.0 + ( v >= 0.0 ? 0.5 : -0.5 ) );
}

struct Shape {
  std::vector<FT_Vector> pts;
  std::vector<char>      tags;
  std::vector<short>     contours;

  Shape& pt( int x, int y, int tag ) {
    pts.push_back( { (FT_Pos)x, (FT_Pos)y } );
    tags.push_back( (char)tag );
    return *this;
  }
  Shape& on( int x, int y )    { return pt( x, y, 1 ); }
  Shape& conic( int x, int y ) { return pt( x, y, 0 ); }
  Shape& cubic( int x, int y ) { return pt( x, y, 2 ); }
  Shape& end() {
    contours.push_back( (short)( pts.size() - 1 ) );
    return *this;
  }

  FT_Outline outline( int fillFlags = 0 ) const {
    FT_Outline o{};
    o.n_contours = (short)contours.size();
    o.n_points   = (short)pts.size();
    o.points     = const_cast<FT_Vector*>( pts.data() );
    o.tags       = const_cast<char*>( tags.data() );
    o.contours   = const_cast<short*>( contours.data() );
    o.flags      = fillFlags;
    return o;
  }

  Shape translated( int dx, int dy ) const {
    Shape s;
    s.pts = pts;
    for ( auto& p : s.pts ) { p.x += dx; p.y += dy; }
    s.tags = tags;
    s.contours = contours;
    return s;
  }

  Shape& append( const Shape& o ) {
    int base = (int)pts.size();
    pts.insert( pts.end(), o.pts.begin(), o.pts.end() );
    tags.insert( tags.end(), o.tags.begin(), o.tags.end() );
    for ( auto c : o.contours )
      contours.push_back( (short)( c + base ) );
    return *this;
  }
};

inline Shape rect( int x0, int y0, int x1, int y1, bool reverse = false ) {
  Shape s;
  if ( !reverse )
    s.on( x0, y0 ).on( x1, y0 ).on( x1, y1 ).on( x0, y1 ).end();
  else
    s.on( x0, y0 ).on( x0, y1 ).on( x1, y1 ).on( x1, y0 ).end();
  return s;
}

// ---------------------------------------------------------------------------
// bands: material that overflows both render pools and forces bisection.
// ---------------------------------------------------------------------------
// Band/bisection boundary lines in device rows for both builds.
//   default pool: initial band height 81 rows -> lines 81, 162; bisections
//                81->40->20->10->5->2.
//   small pool:   initial band height 10 rows -> lines every 10; bisections
//                10->5->2.
inline int bandLine( std::size_t i ) {
  static const int b[] = { 2, 5, 10, 20, 40, 50, 80, 81, 100, 120, 150, 160,
                           1, 4, 9, 11, 19, 21, 39, 41 };
  return b[i % ( sizeof b / sizeof b[0] )];
}

// A tall comb plus full-height bars and a slanted triangle.  Per-row cell
// count dwarfs both cell budgets, so every band overflows and is bisected.
// Ink touches every row, so a dropped band row is visible.
inline Shape bandsPressure( int ox = 0, int oy = 0 ) {
  const int H = px( 162 );

  Shape s;
  s.append( rect( 0, 0, px( 3 ), H ) );
  s.append( rect( px( 88 ), 0, px( 91 ), H ) );
  {
    Shape t;
    t.on( px( 96 ), 0 ).on( px( 104 ), 0 ).on( px( 98 ), H ).end();
    s.append( t );
  }
  for ( int i = 0; i < 20; ++i ) {
    int x0 = px( 6 + 4 * i );
    int b  = bandLine( (std::size_t)i );
    int top = b * 64 + ( i % 2 ? px( 0.3 ) : -px( 0.3 ) );
    if ( i % 4 == 3 ) top = H;
    if ( top < 0 ) top = 0;
    if ( top > H ) top = H;
    s.append( rect( x0, 0, x0 + 64, top ) );
  }
  return s.translated( ox, oy );
}

// A tall cubic wave: N humps on the right boundary, each an inflection whose
// x-extremum lies between two band lines.  Spans 120 rows, crossing the small
// pool's 10-row bands and the default pool's 81-row band.
inline Shape tallCubicWave( int humps, int bulge, int widthPx,
                            int xoff, int yoff ) {
  const int Hpx = 120, H = Hpx * 64;
  const int L = px( 1 ), R = px( 5 + widthPx );

  Shape s;
  s.on( L + xoff, yoff ).on( R + xoff, yoff );
  for ( int i = 0; i < humps; ++i ) {
    int y0 = H * i / humps, y1 = H * ( i + 1 ) / humps;
    int sg = ( i % 2 ) ? -1 : 1;
    int d  = px( 2 );
    s.cubic( R + sg * bulge + xoff, y0 + d + yoff )
     .cubic( R + sg * bulge + xoff, y1 - d + yoff )
     .on( R + xoff, y1 + yoff );
  }
  s.on( L + xoff, H + yoff ).end();
  return s;
}
inline Shape tallConicWave( int humps, int bulge, int widthPx,
                            int xoff, int yoff ) {
  const int Hpx = 120, H = Hpx * 64;
  const int L = px( 1 ), R = px( 5 + widthPx );

  Shape s;
  s.on( L + xoff, yoff ).on( R + xoff, yoff );
  for ( int i = 0; i < humps; ++i ) {
    int y0 = H * i / humps, y1 = H * ( i + 1 ) / humps;
    int sg = ( i % 2 ) ? -1 : 1;
    int cy = y0 + ( y1 - y0 ) / 4;             // vertical offset forces splits
    s.conic( R + sg * bulge + xoff, cy + yoff ).on( R + xoff, y1 + yoff );
  }
  s.on( L + xoff, H + yoff ).end();
  return s;
}

// Arches whose apex sits near a band line; the base is one pixel lower so the
// apex is the true vertical extremum inside a band.
inline Shape conicArch( int x0, int ybase, int wpx, int hsub, int xoff = 0 ) {
  Shape s;
  s.on( x0 + xoff, ybase )
   .conic( x0 + wpx / 2 + xoff, ybase + hsub )
   .on( x0 + wpx + xoff, ybase )
   .on( x0 + wpx + xoff, ybase - 64 )
   .on( x0 + xoff, ybase - 64 ).end();
  return s;
}
inline Shape cubicArch( int x0, int ybase, int wpx, int hsub, int xoff = 0 ) {
  Shape s;
  s.on( x0 + xoff, ybase )
   .cubic( x0 + wpx / 3 + xoff, ybase + hsub )
   .cubic( x0 + 2 * wpx / 3 + xoff, ybase + hsub )
   .on( x0 + wpx + xoff, ybase )
   .on( x0 + wpx + xoff, ybase - 64 )
   .on( x0 + xoff, ybase - 64 ).end();
  return s;
}

// Combs of wavy boundaries plus conic arches straddling band lines.
inline Shape bandsCurves() {
  Shape s;
  // Two tall wavy boundaries crossing every band family: a conic wave at
  // x 1..6 and a cubic wave at x 10..15.  Both have vertical curvature, which
  // is what keeps FreeType's own chord approximation inside 4 gray.
  s.append( tallConicWave( 6, 24, 1, 0, 0 ) );
  s.append( tallCubicWave( 6, 32, 1, px( 9 ), 0 ) );
  // Conic arches whose apexes straddle the 40- and 81-row lines (default-pool
  // initial band line and its first bisection) and the 10-row line (small-pool
  // initial band line).  Started at x >= 18 so no boundary is coincident with
  // a wave (coincident sub-pixel boundaries are outside the contract).
  s.append( conicArch( px( 18 ), 40 * 64 - 20, px( 4 ), 160 ) );
  s.append( conicArch( px( 26 ), 81 * 64 - 20, px( 4 ), 160 ) );
  s.append( conicArch( px( 34 ), 10 * 64 - 20, px( 4 ), 160 ) );
  // Second conic wave in a different phase, also spanning the 81-row line.
  s.append( tallConicWave( 7, 28, 1, px( 42 ), 0 ) );  return s;
}

// ---------------------------------------------------------------------------
// curves: general material.
// ---------------------------------------------------------------------------
// implied conic start: first point is off-curve, last point on-curve.  A low
// curvature wavy boundary with vertical control offsets keeps FreeType's own
// chord approximation inside 4 gray.
inline Shape impliedConicStartOnEnd() {
  const int H = 100 * 64;
  const int L = px( 1 ), R = px( 6 );
  const int xo = 0, yo = 32;
  Shape s;
  s.conic( L + xo, yo );                 // first declared point is off-curve
  s.on( R + xo, yo );
  for ( int i = 0; i < 6; ++i ) {
    int y0 = H * i / 6, y1 = H * ( i + 1 ) / 6;
    int sg = ( i % 2 ) ? -1 : 1;
    s.conic( R + sg * 24 + xo, y0 + ( y1 - y0 ) / 4 + yo )
     .on( R + xo, y1 + yo );
  }
  s.on( L + xo, H + yo ).end();
  return s;
}
// implied conic start: both first and last points are off-curve (FreeType
// starts at their midpoint).  Same gentle wavy boundary.
inline Shape impliedConicStartOffEnd() {
  const int H = 100 * 64;
  const int L = px( 1 ), R = px( 6 );
  const int xo = 0, yo = 32;
  Shape s;
  s.conic( R + xo, yo );                 // first off-curve
  for ( int i = 0; i < 6; ++i ) {
    int y0 = H * i / 6, y1 = H * ( i + 1 ) / 6;
    int sg = ( i % 2 ) ? 1 : -1;
    s.conic( R + sg * 24 + xo, y0 + ( y1 - y0 ) / 4 + yo );
    if ( i + 1 < 6 )
      s.on( R + xo, y1 + yo );
  }
  s.on( L + xo, H + yo );
  s.conic( L + xo, yo );                 // last point off-curve too
  return s;
}
// inner loop with >=2 px clearance; same direction or reversed inner contour
inline Shape innerLoop( bool reverseInner ) {
  Shape s;
  s.append( rect( px( 1 ), px( 1 ), px( 11 ), px( 11 ) ) );
  s.append( rect( px( 4 ), px( 4 ), px( 8 ), px( 8 ), reverseInner ) );
  return s;
}
// outer ring with a hole (>=2 px clearance on all sides)
inline Shape ring() {
  Shape s;
  s.append( rect( px( 1 ), px( 1 ), px( 12 ), px( 9 ) ) );
  s.append( rect( px( 3 ), px( 3 ), px( 10 ), px( 7 ), true ) );
  return s;
}
// two contours sharing a full edge (touching, no area overlap)
inline Shape touchingEdge() {
  Shape s;
  s.append( rect( px( 1 ), px( 1 ), px( 5 ), px( 6 ) ) );
  s.append( rect( px( 5 ), px( 1 ), px( 9 ), px( 6 ) ) );
  return s;
}
// two contours meeting at a single integer corner
inline Shape touchingCorner() {
  Shape s;
  s.append( rect( px( 1 ), px( 1 ), px( 5 ), px( 5 ) ) );
  s.append( rect( px( 5 ), px( 5 ), px( 9 ), px( 9 ) ) );
  return s;
}
// the same integer rectangle declared twice: nonzero fills, even-odd cancels
inline Shape duplicateRect() {
  Shape s;
  s.append( rect( px( 2 ), px( 2 ), px( 8 ), px( 8 ) ) );
  s.append( rect( px( 2 ), px( 2 ), px( 8 ), px( 8 ) ) );
  return s;
}

// ---------------------------------------------------------------------------
// layouts: several disjoint spans per row, plus a gentle conic, and material
// reaching into negative device coordinates.
// ---------------------------------------------------------------------------
inline Shape layoutMixed() {
  Shape s;
  const int H = px( 9 );
  s.append( rect( px( 1 ), px( 1 ), px( 4 ), H ) );
  s.append( rect( px( 6 ), px( 1 ), px( 9 ), H ) );
  s.append( rect( px( 11 ), px( 1 ), px( 14 ), H ) );
  s.append( conicArch( px( 11 ), H, px( 3 ), 160 ) );
  return s;
}

inline Shape negativeShape() {
  Shape s;
  s.append( rect( -px( 3 ), -px( 2 ), px( 1 ), px( 2 ) ) );
  s.append( rect( px( 2 ), 0, px( 6 ), px( 4 ) ) );
  return s;
}

}  // namespace fx
