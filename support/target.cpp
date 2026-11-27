// Gray-rasterizer numerical regression: independent reference, band/bisect
// replay protection, and callback-vs-bitmap layout checks.
//
// Three CASE groups are registered (bands, curves, layouts).  Each runs the
// material on both rasterizer builds (default 16 KiB pool and small 2 KiB pool)
// and compares against the double-precision reference in reference.h.  No main
// is defined here; runner.cpp provides it.
//
// Tolerances (measured, see README):
//   straight outlines   1 gray level  (reference is exact; 1 comes from the
//                                     rasterizer's fixed-point truncation)
//   fixed curve material 4 gray levels (FreeType's own chord approximation)
// The reference's own error is bounded well below 0.5 gray and asserted.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "check.h"
#include "fixtures.h"
#include "raster_api.h"
#include "reference.h"

namespace {

const FT_Raster_Funcs* kApi[2] = { &ft_grays_raster, &ft_grays_raster_small };
const char*            kApiName[2] = { "default-16k", "small-2k" };

// ---------------------------------------------------------------------------
// Bitmap rendering (positive or negative pitch) with sentinel padding.
// ---------------------------------------------------------------------------
// Render an outline into a freshly allocated bitmap.  `pitchSign` selects a
// positive (+1) or negative (-1) pitch; `padLeft`/`padRight`/`padRows` add
// sentinel bytes that must stay untouched.  Returns valid-area pixels
// bottom-up at index (row * width + col).
std::vector<unsigned char> renderBitmap( const FT_Raster_Funcs& api,
                                         const FT_Outline& outline,
                                         int width, int rows, int pitchSign,
                                         int padLeft, int padRight,
                                         int padRows, int flags )
{
  const int stride = padLeft + width + padRight;
  const int nrows  = rows + 2 * padRows;
  // Padding bytes are initialised to the sentinel; the valid pixel area starts
  // fully transparent (0), as the rasterizer only writes covered pixels.
  std::vector<unsigned char> buf( (size_t)stride * nrows, 0xA5 );
  for ( int r = padRows; r < padRows + rows; ++r )
    std::memset( buf.data() + (size_t)r * stride + padLeft, 0, (size_t)width );

  FT_Bitmap bm{};
  bm.width  = (unsigned)width;
  bm.rows   = (unsigned)rows;
  bm.pitch  = pitchSign * stride;
  bm.num_grays = 256;
  bm.pixel_mode = FT_PIXEL_MODE_GRAY;

  const size_t colLo = (size_t)padLeft;
  const size_t colHi = (size_t)( padLeft + width );
  const size_t rowLo = (size_t)padRows;
  const size_t rowHi = (size_t)( padRows + rows );

  // FT_Bitmap.pitch is the byte offset added to go down one physical row, and
  // the buffer is its top row.  ftgrays writes pixel (x, device row y) at
  //   origin - pitch * y + x,  where origin = buffer + (rows-1)*pitch
  // (positive pitch) or origin = buffer (negative pitch).
  // Hence, with device y counted upward from the bitmap's bottom row:
  //   positive pitch: device row y -> physical row (rowHi-1-y)
  //   negative pitch: device row y -> physical row (rowLo+y)
  bm.buffer = buf.data() + rowLo * (size_t)stride + colLo;
  bm.pitch  = pitchSign * stride;
  bm.width  = (unsigned)width;
  bm.rows   = (unsigned)rows;
  bm.num_grays = 256;
  bm.pixel_mode = FT_PIXEL_MODE_GRAY;

  FT_Raster raster;
  if ( api.raster_new( nullptr, &raster ) )
  {
    std::fprintf( stderr, "raster_new failed\n" );
    std::exit( 2 );
  }
  FT_Raster_Params p{};
  p.source = &outline;
  p.target = &bm;
  p.flags  = FT_RASTER_FLAG_AA;
  int err = api.raster_render( raster, &p );
  api.raster_done( raster );
  if ( err )
  {
    std::fprintf( stderr, "raster_render failed (%d)\n", err );
    std::exit( 2 );
  }

  // Copy valid pixels bottom-up using the mapping above.
  std::vector<unsigned char> out( (size_t)width * rows );
  for ( int y = 0; y < rows; ++y )
  {
    size_t phys = ( pitchSign > 0 )
                    ? ( rowHi - 1 - (size_t)y )
                    : ( rowLo + (size_t)y );
    const unsigned char* rowp = buf.data() + phys * (size_t)stride + colLo;
    for ( int x = 0; x < width; ++x )
      out[(size_t)y * width + x] = rowp[x];
  }

  // Every byte outside the valid rectangle (left/right padding and the pad
  // rows above/below) must be untouched.
  for ( size_t i = 0; i < buf.size(); ++i )
  {
    size_t physRow = i / (size_t)stride;
    size_t col = i % (size_t)stride;
    bool inside = ( col >= colLo && col < colHi &&
                    physRow >= rowLo && physRow < rowHi );
    if ( !inside )
      CHECK( buf[i] == 0xA5 );
  }
  return out;
}

// Direct gray_spans rendering, reconstructing a bottom-up image.
struct SpanCollector {
  int width, rows;
  std::vector<unsigned char> img;
  long callbacks = 0;
};

void spanCallback( int y, int count, const FT_Span* spans, void* user )
{
  SpanCollector* sc = (SpanCollector*)user;
  ++sc->callbacks;
  for ( int i = 0; i < count; ++i )
  {
    const FT_Span& s = spans[i];
    // Gray spans overwrite; a later span over the same pixel replaces the
    // value (the rasterizer emits non-overlapping spans per row, but we do
    // not assume one span per callback).
    int x0 = s.x, x1 = s.x + (int)s.len;
    for ( int x = x0; x < x1; ++x )
    {
      if ( x < 0 || x >= sc->width )
        continue;
      if ( y < 0 || y >= sc->rows )
        continue;
      sc->img[(size_t)y * sc->width + x] = s.coverage;
    }
  }
}

std::vector<unsigned char> renderDirect( const FT_Raster_Funcs& api,
                                         const FT_Outline& outline,
                                         int width, int rows,
                                         int clipXMin, int clipYMin,
                                         int clipXMax, int clipYMax )
{
  SpanCollector sc;
  sc.width = width; sc.rows = rows;
  sc.img.assign( (size_t)width * rows, 0 );

  FT_Raster raster;
  if ( api.raster_new( nullptr, &raster ) )
  {
    std::fprintf( stderr, "raster_new failed\n" );
    std::exit( 2 );
  }
  FT_Raster_Params p{};
  p.source     = &outline;
  p.target     = nullptr;
  p.flags      = FT_RASTER_FLAG_AA | FT_RASTER_FLAG_DIRECT;
  p.gray_spans = spanCallback;
  p.user       = &sc;
  p.clip_box.xMin = clipXMin;
  p.clip_box.yMin = clipYMin;
  p.clip_box.xMax = clipXMax;
  p.clip_box.yMax = clipYMax;
  int err = api.raster_render( raster, &p );
  api.raster_done( raster );
  if ( err )
  {
    std::fprintf( stderr, "raster_render direct failed (%d)\n", err );
    std::exit( 2 );
  }
  return sc.img;
}

// ---------------------------------------------------------------------------
// Comparison helpers.
// ---------------------------------------------------------------------------
struct Diff { double maxDiff; int x, y; int got; double ref; };

Diff compareImages( const std::vector<unsigned char>& got,
                    const ref::Result& rr, int width, int rows,
                    double tol, const char* label )
{
  Diff d{ 0.0, 0, 0, 0, 0.0 };
  for ( int y = 0; y < rows; ++y )
    for ( int x = 0; x < width; ++x )
    {
      int g = got[(size_t)y * width + x];
      double r = rr.gray[(size_t)y * width + x];
      double diff = std::fabs( (double)g - r );
      if ( diff > d.maxDiff )
        d = { diff, x, y, g, r };
    }
  if ( d.maxDiff > tol )
  {
    std::fprintf( stderr,
      "CHECK FAILED %s: fixture=%s at pixel (%d,%d) expected=%.2f actual=%d "
      "|diff|=%.2f tol=%.2f\n",
      __FILE__, label, d.x, d.y, d.ref, d.got, d.maxDiff, tol );
    std::exit( 2 );
  }
  ++task_checks();
  return d;
}

// Reference self-check.  The flattening bound certifies that the reference's
// own per-pixel error is well below 0.5 gray:
//   * each accepted chord deviates from the true curve by at most maxErr px;
//   * the total area between the true outline and the flattened polygon is at
//     most areaErr px^2, so the summed gray error over the whole image is at
//     most 256*areaErr, i.e. under 0.5 gray per pixel for any image with at
//     least 256*areaErr/0.5 pixels (always true here).
void checkReferenceBound( const ref::Result& rr, const char* label )
{
  CHECK( rr.maxErr < 2e-3 );                       // px, per-chord deviation
  double npix = (double)rr.width * (double)rr.rows;
  double worstPerPixel = ( npix > 1.0 ) ? ( rr.areaErr * 256.0 / npix ) : 0.0;
  CHECK( worstPerPixel < 0.5 );
  ++task_checks();
  (void)label;
}

}  // namespace

// ===========================================================================
CASE( bands )
{
  // The oversized straight material: must bisect on both builds, and every
  // row's ink must be reproduced.  Tolerance 1 gray (straight edges).
  for ( int a = 0; a < 2; ++a )
  {
    const FT_Raster_Funcs& api = *kApi[a];
    for ( int t = 0; t < 4; ++t )
    {
      // translate by integer and small negative fractions
      static const int dx[4] = { 0, 37, -37, 5 };
      static const int dy[4] = { 0, 0, -22, -13 };
      fx::Shape s = fx::bandsPressure().translated( dx[t], dy[t] );

      const int W = 110, H = 175;
      std::vector<unsigned char> bmp = renderBitmap(
          api, s.outline(), W, H, t % 2 ? -1 : 1, 3, 5, 2, 0 );
      auto rr = ref::render( s.outline(), W, H, 0, 5e-5 );
      checkReferenceBound( rr, "bandsPressure-straight" );
      char label[96];
      std::snprintf( label, sizeof label, "%s doct%d/%d", kApiName[a], dx[t], dy[t] );
      compareImages( bmp, rr, W, H, 1.0, label );
    }
  }

  // Curve material that crosses many bands, plus arches on band lines.
  for ( int a = 0; a < 2; ++a )
  {
    const FT_Raster_Funcs& api = *kApi[a];
    for ( int t = 0; t < 2; ++t )
    {
      fx::Shape s = fx::bandsCurves().translated( t ? -13 : 0, t ? -19 : 0 );
      const int W = 80, H = 200;
      std::vector<unsigned char> bmp =
          renderBitmap( api, s.outline(), W, H, t ? -1 : 1, 2, 4, 1, 0 );
      auto rr = ref::render( s.outline(), W, H, 0, 5e-5 );
      checkReferenceBound( rr, "bandsCurves" );
      char label[96];
      std::snprintf( label, sizeof label, "%s curves-t%d", kApiName[a], t );
      compareImages( bmp, rr, W, H, 4.0, label );
    }
  }
}

// ===========================================================================
static void checkShape( const fx::Shape& s, int W, int H, int fillFlags,
                        double tol, const char* label )
{
  for ( int a = 0; a < 2; ++a )
  {
    const FT_Raster_Funcs& api = *kApi[a];
    std::vector<unsigned char> bmp =
        renderBitmap( api, s.outline( fillFlags ), W, H, a ? -1 : 1, 1, 2, 1, fillFlags );
    auto rr = ref::render( s.outline( fillFlags ), W, H, fillFlags, 5e-5 );
    checkReferenceBound( rr, label );
    compareImages( bmp, rr, W, H, tol, label );
  }
}

CASE( curves )
{
  // Straight-only curve-group material (exact) plus gentle curves (4 gray).
  checkShape( fx::innerLoop( false ), 16, 16, 0, 1.0, "innerLoop/same" );
  checkShape( fx::innerLoop( true  ), 16, 16, 0, 1.0, "innerLoop/reverse" );
  checkShape( fx::innerLoop( false ), 16, 16, ref::evenOddFlag(), 1.0, "innerLoop/same/evenodd" );
  checkShape( fx::innerLoop( true  ), 16, 16, ref::evenOddFlag(), 1.0, "innerLoop/reverse/evenodd" );
  checkShape( fx::ring(), 18, 14, 0, 1.0, "ring/nonzero" );
  checkShape( fx::ring(), 18, 14, ref::evenOddFlag(), 1.0, "ring/evenodd" );
  checkShape( fx::touchingEdge(), 14, 10, 0, 1.0, "touchingEdge" );
  checkShape( fx::touchingCorner(), 14, 14, 0, 1.0, "touchingCorner" );

  // Duplicate integer-aligned rectangle, checked twice under both fill rules.
  for ( int rep = 0; rep < 2; ++rep )
  {
    // nonzero: still completely filled
    {
      fx::Shape s = fx::duplicateRect();
      auto rr = ref::render( s.outline( 0 ), 14, 14, 0, 5e-5 );
      checkReferenceBound( rr, "duplicateRect/nonzero" );
      // Explicit: interior pixels 255, exterior 0 (rectangle spans px 2..8).
      for ( int y = 3; y < 8; ++y )
        for ( int x = 3; x < 8; ++x )
          CHECK( rr.gray[(size_t)y * 14 + x] == 255.0 );
      CHECK( rr.gray[(size_t)1 * 14 + 1] == 0.0 );
      // And the rasterizer on both pools agrees.
      for ( int a = 0; a < 2; ++a )
      {
        std::vector<unsigned char> bmp = renderBitmap(
            *kApi[a], s.outline( 0 ), 14, 14, 1, 1, 1, 1, 0 );
        Diff d = compareImages( bmp, rr, 14, 14, 0.0, "dupRect/nonzero" );
        (void)d;
      }
    }
    // even-odd: cancels completely
    {
      fx::Shape s = fx::duplicateRect();
      auto rr = ref::render( s.outline( ref::evenOddFlag() ), 14, 14,
                             ref::evenOddFlag(), 5e-5 );
      checkReferenceBound( rr, "duplicateRect/evenodd" );
      for ( auto v : rr.gray )
        CHECK( v == 0.0 );
      for ( int a = 0; a < 2; ++a )
      {
        std::vector<unsigned char> bmp = renderBitmap(
            *kApi[a], s.outline( ref::evenOddFlag() ), 14, 14, 1, 1, 1, 1,
            ref::evenOddFlag() );
        compareImages( bmp, rr, 14, 14, 0.0, "dupRect/evenodd" );
      }
    }
  }

  // Implied conic start contours (gentle wavy boundaries).
  checkShape( fx::impliedConicStartOnEnd(), 10, 104, 0, 4.0, "impliedConicStartOnEnd" );
  checkShape( fx::impliedConicStartOffEnd(), 10, 104, 0, 4.0, "impliedConicStartOffEnd" );

  // Tall waves crossing many bands (curves with vertical curvature).
  checkShape( fx::tallConicWave( 6, 24, 1, 0, 0 ), 9, 128, 0, 4.0, "tallConicWave" );
  checkShape( fx::tallCubicWave( 6, 32, 1, fx::px( 9 ), 0 ), 16, 128, 0, 4.0, "tallCubicWave" );
}

// ===========================================================================
CASE( layouts )
{
  fx::Shape base = fx::layoutMixed();

  for ( int a = 0; a < 2; ++a )
  {
    const FT_Raster_Funcs& api = *kApi[a];

    // Bitmap vs DIRECT must agree on the same outline, positive and negative
    // pitch, with padding.  DIRECT uses nonzero fill and a negative clip_box.
    fx::Shape neg = fx::negativeShape();

    // 1) positive pitch bitmap
    std::vector<unsigned char> bmpPos = renderBitmap(
        api, base.outline(), 20, 14, 1, 2, 3, 1, 0 );
    auto rr = ref::render( base.outline(), 20, 14, 0, 5e-5 );
    checkReferenceBound( rr, "layoutMixed" );
    compareImages( bmpPos, rr, 20, 14, 4.0, "layout/bitmap-pos" );

    // 2) negative pitch bitmap must equal positive
    std::vector<unsigned char> bmpNeg = renderBitmap(
        api, base.outline(), 20, 14, -1, 2, 3, 1, 0 );
    for ( size_t i = 0; i < bmpPos.size(); ++i )
      CHECK( bmpPos[i] == bmpNeg[i] );

    // 3) DIRECT gray_spans with a negative-origin clip_box; rebuild image.
    std::vector<unsigned char> direct = renderDirect(
        api, base.outline(), 20, 14, 0, 0, 20, 14 );
    for ( size_t i = 0; i < bmpPos.size(); ++i )
      CHECK( bmpPos[i] == direct[i] );
    // Multi-span rows must actually occur (several disjoint stripes).
    {
      int multitotal = 0;
      for ( int y = 1; y < 13; ++y )
      {
        int transitions = 0, prev = 0;
        for ( int x = 0; x < 20; ++x )
        {
          int v = direct[(size_t)y * 20 + x];
          if ( ( v != 0 ) != ( prev != 0 ) )
            ++transitions;
          prev = v;
        }
        if ( transitions >= 4 )
          ++multitotal;
      }
      CHECK( multitotal > 0 );
    }

    // 4) Left/right clipping and an empty clip.
    std::vector<unsigned char> leftClip = renderDirect(
        api, base.outline(), 20, 14, 0, 0, 8, 14 );
    for ( int y = 0; y < 14; ++y )
      for ( int x = 0; x < 20; ++x )
        if ( x >= 8 )
          CHECK( leftClip[(size_t)y * 20 + x] == 0 );
    std::vector<unsigned char> rightClip = renderDirect(
        api, base.outline(), 20, 14, 6, 0, 20, 14 );
    for ( int y = 0; y < 14; ++y )
      for ( int x = 0; x < 20; ++x )
        if ( x < 6 )
          CHECK( rightClip[(size_t)y * 20 + x] == 0 );
    // Empty clip renders nothing.
    std::vector<unsigned char> emptyClip = renderDirect(
        api, base.outline(), 20, 14, 5, 5, 5, 5 );
    for ( auto v : emptyClip )
      CHECK( v == 0 );

    // 5) Negative-coordinate outline through DIRECT with a clip at origin.
    std::vector<unsigned char> negDirect = renderDirect(
        api, neg.outline(), 10, 8, 0, 0, 10, 8 );
    std::vector<unsigned char> negBmp = renderBitmap(
        api, neg.outline(), 10, 8, 1, 0, 0, 0, 0 );
    // bitmap clamps whole geometry to [0,width)x[0,rows); DIRECT with the
    // same clip must match.
    for ( size_t i = 0; i < negBmp.size(); ++i )
      CHECK( negBmp[i] == negDirect[i] );
  }

  // 6) Interleaved calls across outlines and pools, with different shapes each
  // time: no stale spans/coverage may leak between renders.
  {
    struct Item { fx::Shape shape; int w, h, flags; };
    std::vector<Item> items;
    items.push_back( { fx::innerLoop( false ), 16, 16, 0 } );
    items.push_back( { fx::touchingEdge(), 14, 10, 0 } );
    items.push_back( { fx::layoutMixed(), 20, 14, 0 } );
    items.push_back( { fx::duplicateRect(), 14, 14, ref::evenOddFlag() } );
    items.push_back( { fx::negativeShape(), 10, 8, 0 } );
    items.push_back( { fx::ring(), 18, 14, ref::evenOddFlag() } );

    for ( size_t k = 0; k < items.size(); ++k )
    {
      const Item& it = items[k];
      const FT_Raster_Funcs& api = *kApi[k % 2];
      // DIRECT result
      std::vector<unsigned char> d = renderDirect(
          api, it.shape.outline( it.flags ), it.w, it.h, 0, 0, it.w, it.h );
      // Bitmap result of the same item
      std::vector<unsigned char> b = renderBitmap(
          api, it.shape.outline( it.flags ), it.w, it.h, k % 2 ? -1 : 1, 1, 2, 1, it.flags );
      for ( size_t i = 0; i < d.size(); ++i )
        CHECK( b[i] == d[i] );
      // Reference agreement
      auto rr2 = ref::render( it.shape.outline( it.flags ), it.w, it.h, it.flags, 5e-5 );
      checkReferenceBound( rr2, "layout/interleaved" );
      compareImages( b, rr2, it.w, it.h, k == 2 ? 4.0 : 1.0, "layout/interleaved" );
    }
  }

  // 7) Outline and its raw arrays must be untouched by rendering.
  {
    fx::Shape s = fx::layoutMixed();
    FT_Outline before = s.outline( 0 );
    std::vector<FT_Vector> p0( s.pts.begin(), s.pts.end() );
    std::vector<char> t0( s.tags );
    std::vector<short> c0( s.contours );

    renderBitmap( *kApi[0], before, 20, 14, 1, 1, 2, 1, 0 );
    renderBitmap( *kApi[1], before, 20, 14, -1, 1, 2, 1, 0 );
    (void)renderDirect( *kApi[0], before, 20, 14, 0, 0, 20, 14 );

    CHECK( before.n_points == (short)p0.size() );
    CHECK( before.n_contours == (short)c0.size() );
    for ( int i = 0; i < before.n_points; ++i )
    {
      CHECK( before.points[i].x == p0[(size_t)i].x );
      CHECK( before.points[i].y == p0[(size_t)i].y );
      CHECK( before.tags[i] == t0[(size_t)i] );
    }
    for ( int i = 0; i < before.n_contours; ++i )
      CHECK( before.contours[i] == c0[(size_t)i] );
  }
}
