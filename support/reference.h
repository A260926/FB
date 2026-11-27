// Independent double-precision reference for the FreeType gray rasterizer.
//
// This file calls no FreeType rendering code: no outline decomposition, no
// curve splitting, no winding or coverage routine.  It treats the raw
// FT_Outline arrays (points / tags / contours) as plain data, builds directed
// geometric primitives itself and integrates pixel coverage in `double`.
//
// Coordinate and coverage model
// -----------------------------
// Outline coordinates are 26.6 fixed point; device space is v / 64.  Pixel
// (col x, row r) is the half-open box [x, x+1) x [r, r+1), where device y
// increases upward from bitmap row 0 (the bottom row).  Coverage is the area
// of the filled region inside that box.  Following the rasterizer, one full
// pixel maps to 256 coverage units, clamped to 255, so the returned gray value
// is min(256 * area, 255) (an idealising model; the rasterizer's own fixed
// point quantization adds at most ~1 unit).
//
// Straight edges are exact.  For every row the reference collects all x
// events (edge endpoint x's, edge/row-boundary crossings and integer column
// boundaries).  Between two consecutive events the vertical inside length
// H(x) is affine and the winding number is constant, so H(midpoint) times the
// event spacing integrates the row exactly.  The winding number at the bottom
// of the row is the signed sum of crossings of the vertical line below it;
// crossing signs are swept upward across the row.  Inside intervals come from
// that signed topology, never from summing absolute edge areas, so holes,
// reversed contours and nested contours combine correctly.
//
// Curves are flattened by adaptive de Casteljau subdivision in double
// precision.  A chord is accepted when every control point lies within `tol`
// of the chord; the curve lies in the convex hull of its controls, so the
// curve-to-chord distance is likewise bounded by that value.  The reference
// reports
//   maxErr  = largest accepted per-chord geometric deviation (pixels), and
//   areaErr = sum over chords of (chord length * chord deviation),
// a strict upper bound on the area between the true outline and the flattened
// polygon.  In gray units the total error is at most 256 * areaErr; with the
// default tolerance of 1e-4 px this is far below half a gray level per pixel.
//
// Contract limits (documented in README): only simple, non self-intersecting
// contours are validated, with distinct boundaries at least two device pixels
// apart.  Overlapping sub-pixel boundaries and coincident-vertex
// self-intersections are outside the model; test material avoids them (the one
// intentional exception is a pair of exactly duplicated integer-aligned
// rectangles, whose fill-rule behaviour is exact and unambiguous).

#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <vector>

#include "raster_api.h"

namespace ref {

struct Pt { double x, y; };

// A directed geometric primitive in integer 26.6 coordinates.
struct Seg {
  enum Kind { Line, Conic, Cubic } kind;
  int x0, y0, x1, y1;       // start, end
  int cx0, cy0;             // conic control, or cubic control 1
  int cx1, cy1;             // cubic control 2
  const char* src;          // diagnostic label
};

inline double du( int v ) { return (double)v / 64.0; }

// ---------------------------------------------------------------------------
// Raw outline -> directed primitives.  Mirrors the documented FreeType
// contour semantics (including a contour that begins with one or two off-curve
// points) but as plain data manipulation, not a library call.
// ---------------------------------------------------------------------------
inline std::vector<std::vector<Seg>>
outlineSegments( const FT_Outline& ol )
{
  std::vector<std::vector<Seg>> out;
  if ( ol.n_contours <= 0 || ol.n_points <= 0 )
    return out;

  int first = 0;
  for ( int n = 0; n < ol.n_contours; ++n )
  {
    int last = ol.contours[n];
    if ( last < first || last >= ol.n_points )
      continue;

    std::vector<Seg> c;
    FT_Pos px = ol.points[first].x, py = ol.points[first].y;
    FT_Pos sx = px, sy = py;      // contour start
    int    p  = first;
    char   t  = ol.tags[p];
    int    limit = last;          // last index consumed by the walk
    bool   closedEarly = false;

    if ( t == 0 )                  // FT_CURVE_TAG_CONIC
    {
      if ( ol.tags[last] == 1 )    // last on-curve: start there, skip it
      {
        sx = ol.points[last].x; sy = ol.points[last].y;
        px = sx; py = sy;
        limit = last - 1;
      }
      else
      {
        // both end points off-curve: start at their midpoint, and the
        // trailing off-curve point stays the control of the closing arc
        sx = ( ol.points[first].x + ol.points[last].x ) / 2;
        sy = ( ol.points[first].y + ol.points[last].y ) / 2;
        px = sx; py = sy;
      }
      p = first - 1;               // the walk increments before reading
    }

    auto lineTo = [&]( FT_Pos ex, FT_Pos ey ) {
      c.push_back( { Seg::Line, (int)px, (int)py, (int)ex, (int)ey,
                     0, 0, 0, 0, nullptr } );
      px = ex; py = ey;
    };
    auto conicTo = [&]( FT_Pos qx, FT_Pos qy, FT_Pos ex, FT_Pos ey ) {
      c.push_back( { Seg::Conic, (int)px, (int)py, (int)ex, (int)ey,
                     (int)qx, (int)qy, 0, 0, nullptr } );
      px = ex; py = ey;
    };
    auto cubicTo = [&]( FT_Pos ax, FT_Pos ay, FT_Pos bx, FT_Pos by,
                        FT_Pos ex, FT_Pos ey ) {
      c.push_back( { Seg::Cubic, (int)px, (int)py, (int)ex, (int)ey,
                     (int)ax, (int)ay, (int)bx, (int)by, nullptr } );
      px = ex; py = ey;
    };

    while ( p < limit )
    {
      ++p;
      t = ol.tags[p];
      if ( t == 1 )                // on-curve
      {
        lineTo( ol.points[p].x, ol.points[p].y );
        continue;
      }
      if ( t == 0 )                // conic
      {
        FT_Pos qx = ol.points[p].x, qy = ol.points[p].y;

        for ( ;; )
        {
          if ( p < limit )
          {
            ++p;
            t = ol.tags[p];
            if ( t == 1 )
            {
              conicTo( qx, qy, ol.points[p].x, ol.points[p].y );
              break;
            }
            if ( t != 0 )
              break;               // malformed
            FT_Pos vx = ol.points[p].x, vy = ol.points[p].y;
            conicTo( qx, qy, ( qx + vx ) / 2, ( qy + vy ) / 2 );
            qx = vx; qy = vy;
            continue;
          }
          conicTo( qx, qy, sx, sy );
          closedEarly = true;
          break;
        }
        if ( closedEarly )
          break;
        continue;
      }
      // cubic
      if ( p + 1 > limit || ol.tags[p + 1] != 2 )
        break;
      FT_Pos ax = ol.points[p].x, ay = ol.points[p].y;
      FT_Pos bx = ol.points[p + 1].x, by = ol.points[p + 1].y;
      p += 2;
      if ( p <= limit )
      {
        cubicTo( ax, ay, bx, by, ol.points[p].x, ol.points[p].y );
        continue;
      }
      cubicTo( ax, ay, bx, by, sx, sy );
      closedEarly = true;
      break;
    }

    if ( !closedEarly )
      lineTo( sx, sy );            // explicit closure
    if ( !c.empty() )
      out.push_back( std::move( c ) );
    first = last + 1;
  }
  return out;
}

// ---------------------------------------------------------------------------
// Adaptive flattening.
// ---------------------------------------------------------------------------
inline double distToChord( Pt p, Pt a, Pt b )
{
  double dx = b.x - a.x, dy = b.y - a.y;
  double len2 = dx * dx + dy * dy;
  if ( len2 == 0.0 )
    return std::hypot( p.x - a.x, p.y - a.y );
  return std::fabs( dx * ( a.y - p.y ) - ( a.x - p.x ) * dy ) / std::sqrt( len2 );
}

struct Flat {
  std::vector<Pt> pts;      // polyline; closed contours repeat the start
  double maxErr  = 0.0;     // largest per-chord deviation
  double areaErr = 0.0;     // sum of chord length * chord deviation

  void emit( Pt a, Pt b, double e )
  {
    maxErr  = std::max( maxErr, e );
    areaErr += std::hypot( b.x - a.x, b.y - a.y ) * e;
    if ( pts.empty() )
      pts.push_back( a );
    pts.push_back( b );
  }

  static Pt mid( Pt a, Pt b ) { return { (a.x + b.x) / 2, (a.y + b.y) / 2 }; }

  void conic( Pt p0, Pt q, Pt p1, double tol, int depth )
  {
    double e = ( depth > 48 ) ? std::numeric_limits<double>::infinity()
                              : distToChord( q, p0, p1 );
    if ( e <= tol )
    {
      emit( p0, p1, e );
      return;
    }
    Pt a = mid( p0, q ), b = mid( q, p1 ), m = mid( a, b );
    conic( p0, a, m, tol, depth + 1 );
    conic( m, b, p1, tol, depth + 1 );
  }

  void cubic( Pt p0, Pt c0, Pt c1, Pt p1, double tol, int depth )
  {
    double e = ( depth > 48 ) ? std::numeric_limits<double>::infinity()
              : std::max( distToChord( c0, p0, p1 ),
                          distToChord( c1, p0, p1 ) );
    if ( e <= tol )
    {
      emit( p0, p1, e );
      return;
    }
    Pt a = mid( p0, c0 ), b = mid( c0, c1 ), c = mid( c1, p1 );
    Pt m0 = mid( a, b ), m1 = mid( b, c ), m = mid( m0, m1 );
    cubic( p0, a, m0, m, tol, depth + 1 );
    cubic( m, m1, c, p1, tol, depth + 1 );
  }

  void add( const Seg& s, double tol )
  {
    Pt p0{ du( s.x0 ), du( s.y0 ) }, p1{ du( s.x1 ), du( s.y1 ) };
    if ( s.kind == Seg::Line )
      emit( p0, p1, 0.0 );
    else if ( s.kind == Seg::Conic )
      conic( p0, Pt{ du( s.cx0 ), du( s.cy0 ) }, p1, tol, 0 );
    else
      cubic( p0, Pt{ du( s.cx0 ), du( s.cy0 ) },
             Pt{ du( s.cx1 ), du( s.cy1 ) }, p1, tol, 0 );
  }
};

// ---------------------------------------------------------------------------
// Whole-outline flattening, with the error statistics above.
// ---------------------------------------------------------------------------
struct Geometry {
  std::vector<Pt> e0, e1;   // directed edge endpoints (device space)
  double maxErr  = 0.0;
  double areaErr = 0.0;

  size_t size() const { return e0.size(); }
};

inline Geometry flattenOutline( const FT_Outline& ol, double curveTol )
{
  Geometry g;
  std::vector<std::vector<Seg>> cs = outlineSegments( ol );
  for ( auto& c : cs )
  {
    Flat f;
    for ( auto& s : c )
      f.add( s, curveTol );
    g.maxErr  = std::max( g.maxErr, f.maxErr );
    g.areaErr += f.areaErr;

    const std::vector<Pt>& v = f.pts;
    if ( v.size() < 2 )
      continue;
    size_t n = v.size();
    bool closed = ( v.back().x == v.front().x && v.back().y == v.front().y );
    size_t m = closed ? n - 1 : n;
    for ( size_t i = 0; i < m; ++i )
    {
      const Pt& a = v[i];
      const Pt& b = v[( i + 1 ) % n];
      if ( a.x != b.x || a.y != b.y )
      {
        g.e0.push_back( a );
        g.e1.push_back( b );
      }
    }
  }
  return g;
}

// ---------------------------------------------------------------------------
// Reference result.  gray is min(floor(256 * area), 255), the idealization the
// rasterizer quantizes to (its own fixed-point truncation is bounded by one
// gray level; see README).  rawGray keeps the untruncated 256*area value
// (0..256) so callers can reason about sub-level error.
// ---------------------------------------------------------------------------
struct Result {
  int width = 0, rows = 0;
  std::vector<double> rawGray;  // 256 * coverage area, truncated only at 256
  std::vector<double> gray;     // min(floor(rawGray), 255)
  double maxErr  = 0.0;         // px, geometric flattening error
  double areaErr = 0.0;         // px^2, total flattening area error
};

inline Result render( const FT_Outline& ol, int width, int rows, int flags,
                      double curveTol = 1e-4 )
{
  Result res;
  res.width = width;
  res.rows  = rows;
  res.rawGray.assign( (size_t)width * (size_t)rows, 0.0 );
  if ( width <= 0 || rows <= 0 )
    return res;

  const bool evenOdd = ( flags & FT_OUTLINE_EVEN_ODD_FILL ) != 0;
  Geometry g = flattenOutline( ol, curveTol );
  res.maxErr  = g.maxErr;
  res.areaErr = g.areaErr;
  const size_t E = g.size();
  if ( E == 0 )
  {
    res.gray = res.rawGray;
    return res;
  }

  std::vector<double> ev;
  std::vector<int>    wb;
  std::vector<double> yb;

  for ( int r = 0; r < rows; ++r )
  {
    const double ry0 = (double)r, ry1 = (double)( r + 1 );

    ev.clear();
    ev.reserve( (size_t)width + 4 * E + 2 );
    for ( int x = 0; x <= width; ++x )
      ev.push_back( (double)x );
    for ( size_t i = 0; i < E; ++i )
    {
      double x0 = g.e0[i].x, y0 = g.e0[i].y;
      double x1 = g.e1[i].x, y1 = g.e1[i].y;
      ev.push_back( x0 );
      ev.push_back( x1 );
      double loY = std::min( y0, y1 ), hiY = std::max( y0, y1 );
      if ( hiY < ry0 || loY > ry1 || y0 == y1 )
        continue;
      double dy = y1 - y0;
      double t0 = ( ry0 - y0 ) / dy;
      double t1 = ( ry1 - y0 ) / dy;
      if ( t0 > 0.0 && t0 < 1.0 ) ev.push_back( x0 + t0 * ( x1 - x0 ) );
      if ( t1 > 0.0 && t1 < 1.0 ) ev.push_back( x0 + t1 * ( x1 - x0 ) );
    }
    std::sort( ev.begin(), ev.end() );
    ev.erase( std::unique( ev.begin(), ev.end() ), ev.end() );
    ev.erase( std::remove_if( ev.begin(), ev.end(),
                              [&]( double v ) { return v < 0.0 || v > width; } ),
              ev.end() );

    for ( size_t k = 0; k + 1 < ev.size(); ++k )
    {
      double a = ev[k], b = ev[k + 1];
      if ( b <= a )
        continue;
      double xm = ( a + b ) / 2.0;
      int col = (int)std::floor( xm );
      if ( col < 0 || col >= width )
        continue;

      // Signed crossings of the vertical line x = xm, plus the winding just
      // below the row.
      struct Crossing { double y; int sign; };
      static thread_local std::vector<Crossing> cr;
      cr.clear();
      int wBottom = 0;

      for ( size_t i = 0; i < E; ++i )
      {
        double x0 = g.e0[i].x, y0 = g.e0[i].y;
        double x1 = g.e1[i].x, y1 = g.e1[i].y;
        if ( x0 == x1 )
          continue;                       // vertical edge: never crossed
        double loX = std::min( x0, x1 ), hiX = std::max( x0, x1 );
        if ( xm <= loX || xm >= hiX )
          continue;                       // half-open: endpoints excluded
        double y = y0 + ( xm - x0 ) / ( x1 - x0 ) * ( y1 - y0 );
        int s = ( x1 > x0 ) ? 1 : -1;
        if ( y <= ry0 )
          wBottom += s;
        else if ( y < ry1 )
          cr.push_back( { y, s } );
      }

      std::sort( cr.begin(), cr.end(),
                 []( const Crossing& u, const Crossing& v ) { return u.y < v.y; } );

      int    w   = wBottom;
      double len = 0.0;
      double ylo = ry0;
      auto inside = [&]( int ww ) {
        return evenOdd ? ( ( ( ww < 0 ? -ww : ww ) & 1 ) != 0 ) : ( ww != 0 );
      };
      for ( size_t i = 0; i < cr.size(); ++i )
      {
        double yhi = cr[i].y;
        if ( yhi > ylo && inside( w ) )
          len += yhi - ylo;
        w += cr[i].sign;
        ylo = yhi;
      }
      if ( ry1 > ylo && inside( w ) )
        len += ry1 - ylo;

      res.rawGray[(size_t)r * (size_t)width + col] += len * ( b - a ) * 256.0;
    }
  }

  res.gray.resize( res.rawGray.size() );
  for ( size_t i = 0; i < res.rawGray.size(); ++i )
  {
    double v = res.rawGray[i];
    if ( v > 256.0 ) v = 256.0;
    if ( v < 0.0 )   v = 0.0;
    res.gray[i] = std::floor( v );
    if ( res.gray[i] > 255.0 ) res.gray[i] = 255.0;
  }
  return res;
}

inline int evenOddFlag() { return (int)FT_OUTLINE_EVEN_ODD_FILL; }

}  // namespace ref
