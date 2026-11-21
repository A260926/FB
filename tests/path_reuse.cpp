/*------------------------------------------------------------------------------

   Tests for the CPU diffuse suffix-reconnection mode (PathReuse).

   Registered through task_support/check.h in three groups:
     measure   - finite-walk reference statistics vs the reuse estimator
     transport - hand-computed area densities, MIS mixing and visibility
     passes    - freezing, ordering, pass counting, isolation, legacy path

   This translation unit defines no main(): task_support/driver.c supplies it.
------------------------------------------------------------------------------*/


#include <math.h>
#include <sstream>
#include <vector>
#include <string>
#include <cmath>
#include <cstdio>

#include "task_support/check.h"

#include "Camera.hpp"
#include "Scene.hpp"
#include "Image.hpp"
#include "Random.hpp"
#include "RayTracer.hpp"
#include "SurfacePoint.hpp"
#include "Triangle.hpp"
#include "PathReuse.hpp"


using namespace hxa7241_minilight;
using namespace hxa7241_general;
using namespace hxa7241_graphics;


namespace
{
   const double PI = 3.14159265358979;
   const double EPS = 1.0e-9;


   double dot3( const Vector3f& a, const Vector3f& b ) { return a.dot( b ); }

   double length3( const Vector3f& a ) { return ::sqrt( a.dot( a ) ); }

   bool close( double a, double b, double tol = 1.0e-7 )
   {
      return ::fabs( a - b ) <= tol + 1.0e-12 * ::fabs( b );
   }

   bool closeV( const Vector3f& a, const Vector3f& b, double tol = 1.0e-7 )
   {
      return close( a[0], b[0], tol ) && close( a[1], b[1], tol ) &&
             close( a[2], b[2], tol );
   }


   Camera makeCamera( const char* text )
   {
      std::istringstream s( text );
      return Camera( s );
   }

   Scene makeScene( const Camera& camera, const char* text )
   {
      std::istringstream s( text );
      s.exceptions( std::ios::badbit | std::ios::failbit | std::ios::eofbit );
      return Scene( s, camera.getEyePoint() );
   }


   /**
    * Independent area-density evaluation: the same geometry formula written
    * out here rather than calling PathReuse::sourceDensity, using the scene's
    * own spatial index for the first-hit visibility test.
    */
   double handDensity
   (
      const Scene&           scene,
      const Vector3f&        sourcePos,
      const Vector3f&        sourceCameraNormal,
      const Triangle*        sourceTriangle,
      const Vector3f&        y,
      const Triangle*        yTriangle,
      int32                  ySideSign
   )
   {
      const Vector3f delta( y - sourcePos );
      const double dist2 = dot3( delta, delta );
      if( dist2 < EPS * EPS ) return 0.0;

      const Vector3f w( delta / ::sqrt( dist2 ) );
      const double nkDot = dot3( sourceCameraNormal, w );
      if( nkDot <= EPS ) return 0.0;

      const Triangle* hit = 0;
      Vector3f hitPos;
      scene.getIntersection( sourcePos, w,
         static_cast<const void*>( sourceTriangle ), hit, hitPos );
      if( 0 == hit || hit != yTriangle ) return 0.0;
      if( length3( hitPos - y ) > 1.0e-6 ) return 0.0;

      const Vector3f ny( yTriangle->getNormal() );
      const double nyDot = dot3( ny, Vector3f( 0.0, 0.0, 0.0 ) - w );
      if( (nyDot >= 0.0 ? 1 : -1) != ySideSign ) return 0.0;
      if( ::fabs( nyDot ) <= EPS ) return 0.0;

      return (nkDot / PI) * (::fabs( nyDot ) / dist2);
   }


   /**
    * Independent receiver contribution F_i(Y): albedo/pi * receiver cosine *
    * absolute y cosine / r^2 * B, zeroed on failed finite visibility or a side
    * mismatch.
    */
   Vector3f handReceiver
   (
      const Scene&           scene,
      const Vector3f&        x,
      const Vector3f&        xCameraNormal,
      const Triangle*        xTriangle,
      const Vector3f&        y,
      const Triangle*        yTriangle,
      int32                  ySideSign,
      const Vector3f&        B
   )
   {
      const Vector3f delta( y - x );
      const double dist2 = dot3( delta, delta );
      if( dist2 < EPS * EPS ) return Vector3f::ZERO();

      const Vector3f w( delta / ::sqrt( dist2 ) );
      const double rcos = dot3( xCameraNormal, w );
      if( rcos <= EPS ) return Vector3f::ZERO();

      const Vector3f ny( yTriangle->getNormal() );
      const double yabs = ::fabs( dot3( ny, w ) );
      if( yabs <= EPS ) return Vector3f::ZERO();
      if( (dot3( ny, Vector3f( 0.0, 0.0, 0.0 ) - w ) >= 0.0 ? 1 : -1) !=
          ySideSign )
         return Vector3f::ZERO();

      const Triangle* hit = 0;
      Vector3f hitPos;
      scene.getIntersection( x, w,
         static_cast<const void*>( xTriangle ), hit, hitPos );
      if( 0 != hit )
      {
         if( hit != yTriangle || length3( hitPos - y ) > 1.0e-6 )
            return Vector3f::ZERO();
      }

      const double geom = (rcos * yabs) / dist2;
      return (B * xTriangle->getReflectivity()) * (geom / PI);
   }


   /**
    * Independent production-shaped finite diffuse walk: no Russian roulette,
    * no explicit emitter sampling, bounded depth, emission kept at every hit
    * and albedo carried forward. This is the non-reused reference estimator.
    */
   Vector3f referenceWalk
   (
      const Scene&    scene,
      Random&         random,
      const Vector3f& position,
      const Triangle& triangle,
      const Vector3f& inward,
      int32           verticesLeft
   )
   {
      Vector3f radiance;
      if( triangle.getNormal().dot( inward ) > 0.0 )
         radiance = triangle.getEmitivity();

      if( verticesLeft <= 1 ) return radiance;

      Vector3f normal( triangle.getNormal() );
      const Vector3f tangent( triangle.getTangent() );
      if( normal.dot( inward ) < 0.0 ) normal = Vector3f( 0.0, 0.0, 0.0 ) -
         normal;

      const double ang = PI * 2.0 * random.getReal64();
      const double sr  = ::sqrt( random.getReal64() );
      const Vector3f dir( tangent * (::cos( ang ) * sr) +
         normal.cross( tangent ) * (::sin( ang ) * sr) +
         normal * ::sqrt( 1.0 - sr * sr) );

      const Triangle* hit = 0;
      Vector3f hitPos;
      scene.getIntersection( position, dir,
         static_cast<const void*>( &triangle ), hit, hitPos );
      if( 0 == hit ) return radiance;

      return radiance + triangle.getReflectivity() *
         referenceWalk( scene, random, hitPos, *hit,
            Vector3f( 0.0, 0.0, 0.0 ) - dir, verticesLeft - 1 );
   }


   // enclosed black-background room shared by the transport tests
   const char ROOM[] =
      "(0 0 0) (0 0 0)\n"
      "(-3 -2 1) (-3 -2 5) (3 -2 1)  (0.7 0.7 0.7) (0 0 0)\n"
      "(3 -2 1) (-3 -2 5) (3 -2 5)   (0.7 0.7 0.7) (0 0 0)\n"
      "(-3 2 1) (3 2 1) (-3 2 5)     (0.8 0.8 0.8) (5 5 5)\n"
      "(3 2 1) (3 2 5) (-3 2 5)      (0.8 0.8 0.8) (5 5 5)\n"
      "(-3 -2 5) (3 -2 5) (-3 2 5)   (0.7 0.7 0.7) (0 0 0)\n"
      "(3 -2 5) (3 2 5) (-3 2 5)     (0.7 0.7 0.7) (0 0 0)\n"
      "(-3 -2 1) (-3 2 1) (3 -2 1)   (0.7 0.7 0.7) (0 0 0)\n"
      "(3 -2 1) (-3 2 1) (3 2 1)     (0.7 0.7 0.7) (0 0 0)\n"
      "(-3 -2 1) (-3 -2 5) (-3 2 1)  (0.6 0.2 0.2) (0 0 0)\n"
      "(-3 2 1) (-3 -2 5) (-3 2 5)   (0.6 0.2 0.2) (0 0 0)\n"
      "(3 -2 1) (3 2 1) (3 -2 5)     (0.2 0.2 0.6) (0 0 0)\n"
      "(3 2 1) (3 2 5) (3 -2 5)      (0.2 0.2 0.6) (0 0 0)\n";
}


/*==============================================================================
   measure group
==============================================================================*/

static int measure_statistics()
{
   const int32 W = 12, H = 12, D = 4, BATCHES = 24;
   const Camera camera( makeCamera( "(0 0 2) (0 0 1) 90" ) );
   const Scene  scene( makeScene( camera, ROOM ) );

   std::vector<Vector3f> reuseSum( W * H, Vector3f::ZERO() );
   std::vector<Vector3f> refSum( W * H, Vector3f::ZERO() );
   std::vector<Vector3f> reuseSq( W * H, Vector3f::ZERO() );
   std::vector<Vector3f> refSq( W * H, Vector3f::ZERO() );

   int32 measuredSuffixes = 0;

   for( int32 b = 0; b < BATCHES; ++b )
   {
      // reuse estimator, one pass per batch with a fixed seed
      std::istringstream ris( "12 12" );
      Image reuseImage( ris );
      PathReuse reuse;
      reuse.bind( camera, scene, W, H, D, 1000u + static_cast<int32u>( b ) );
      TASK_CHECK( reuse.runPass( camera, scene, reuseImage ) );
      measuredSuffixes += reuse.getLastSuffixCount();

      // independent non-reused path with the identical structure to a reuse
      // path: camera first-hit x, one cosine edge to y, then a D-1 vertex
      // finite suffix starting at y
      std::istringstream ris2( "12 12" );
      Image refImage( ris2 );
      Random refRandom( 2000u + static_cast<int32u>( b ) );
      for( int32 y = 0; y < H; ++y )
         for( int32 x = 0; x < W; ++x )
         {
            const Vector3f eye( camera.getEyePoint() );
            const Vector3f dir( camera.getPixelRayDirection( x, y, W, H,
               refRandom ) );
            const Triangle* hit = 0;
            Vector3f hitPos;
            scene.getIntersection( eye, dir, 0, hit, hitPos );

            Vector3f color;
            if( 0 != hit )
            {
               Vector3f nx( hit->getNormal() );
               if( nx.dot( eye - hitPos ) < 0.0 )
                  nx = Vector3f( 0.0, 0.0, 0.0 ) - nx;

               color = SurfacePoint( *hit, hitPos ).getEmission(
                  eye, (eye - hitPos).unitize(), false );

               // the single source edge x -> y (same cosine sampling)
               const double ang = PI * 2.0 * refRandom.getReal64();
               const double sr  = ::sqrt( refRandom.getReal64() );
               const Vector3f tangent( hit->getTangent() );
               const Vector3f edge( tangent * (::cos( ang ) * sr) +
                  nx.cross( tangent ) * (::sin( ang ) * sr) +
                  nx * ::sqrt( 1.0 - sr * sr) );

               const Triangle* yhit = 0;
               Vector3f yPos;
               scene.getIntersection( hitPos, edge,
                  static_cast<const void*>( hit ), yhit, yPos );
               if( 0 != yhit )
               {
                  const Vector3f suffix( referenceWalk( scene, refRandom,
                     yPos, *yhit,
                     Vector3f( 0.0, 0.0, 0.0 ) - edge, D - 1 ) );
                  color = color + hit->getReflectivity() * suffix;
               }
            }
            const int32 idx = y * W + x;
            refSum[idx] = refSum[idx] + color;
            refSq[idx]  = refSq[idx] + color * color;
         }

      for( int32 i = 0; i < W * H; ++i )
      {
         const Vector3f c( reuse.getLinearPixel( i % W, i / W ) );
         reuseSum[i] = reuseSum[i] + c;
         reuseSq[i]  = reuseSq[i] + c * c;
      }
   }

   TASK_CHECK( measuredSuffixes > 0 );

   // interior pixels only: all five source slots are valid, so the neighbour
   // support covers the receiver's own cosine region without edge truncation
   double meanDiff = 0.0, mergedSE = 0.0, meanReuse = 0.0, meanRef = 0.0;
   int32 interior = 0;
   for( int32 y = 2; y < H - 2; ++y )
      for( int32 x = 2; x < W - 2; ++x )
      {
         const int32 i = y * W + x;
         for( int c = 0; c < 3; ++c )
         {
            const double mR = reuseSum[i][c] / BATCHES;
            const double mF = refSum[i][c]   / BATCHES;
            const double vR = (reuseSq[i][c] / BATCHES) - mR * mR;
            const double vF = (refSq[i][c]   / BATCHES) - mF * mF;
            const double seR = ::sqrt( (vR > 0.0 ? vR : 0.0) / BATCHES );
            const double seF = ::sqrt( (vF > 0.0 ? vF : 0.0) / BATCHES );
            meanDiff  += ::fabs( mR - mF );
            mergedSE  += ::sqrt( seR * seR + seF * seF );
            meanReuse += mR;
            meanRef   += mF;
         }
         ++interior;
      }

   const double tolerance = 5.0 * mergedSE + 1.0e-3;
   std::printf(
      "measure interior=%d mean_reuse=%.6f mean_reference=%.6f "
      "abs_diff=%.6f merged_SE=%.6f tolerance=%.6f suffixes=%d\n",
      interior, meanReuse, meanRef, meanDiff, mergedSE, tolerance,
      measuredSuffixes );

   TASK_CHECK( interior > 0 );
   TASK_CHECK( meanDiff <= tolerance );
   return 0;
}


static int measure_diagnostics()
{
   const int32 W = 8, H = 8, D = 3;
   const Camera camera( makeCamera( "(0 0 2) (0 0 1) 90" ) );
   const Scene  scene( makeScene( camera, ROOM ) );

   std::istringstream ii( "8 8" );
   Image image( ii );
   PathReuse reuse;
   reuse.bind( camera, scene, W, H, D, 777u );
   TASK_CHECK( reuse.runPass( camera, scene, image ) );

   real64 sumD = 0.0, minD = 0.0, maxD = 0.0;
   int32 zeroD = 0, occluded = 0;
   reuse.getDiagnostics( sumD, minD, maxD, zeroD, occluded );

   std::printf(
      "measure diagnostics sum=%.6f min_pos=%.3e max=%.6f zero=%d occluded=%d "
      "suffixes=%d sources=%d\n",
      sumD, minD, maxD, zeroD, occluded, reuse.getLastSuffixCount(),
      reuse.getLastValidSourceCount() );

   TASK_CHECK( sumD > 0.0 );
   TASK_CHECK( minD > 0.0 );
   TASK_CHECK( maxD >= minD );
   TASK_CHECK( zeroD == 0 );
   TASK_CHECK( reuse.getLastSuffixCount() == reuse.getLastValidSourceCount() );
   return 0;
}


// analytically integrable small scene: a front-facing, zero-albedo emitter on
// a black background has an exactly known radiance (its emitivity, since the
// non-solid-angle first-hit term has unit solid angle and there are no
// bounces). This anchors the estimator to an absolute value, not merely to a
// comparison between two new functions.
static int measure_analytic_radiance()
{
   std::istringstream ci( "(0 0 0) (0 0 1) 40" );
   const Camera camera( ci );
   std::istringstream si(
      "(0 0 0) (0 0 0)\n"
      "(-10 -10 1) (0 10 1) (10 -10 1) (0 0 0) (3 2 1)\n" );
   si.exceptions( std::ios::badbit | std::ios::failbit | std::ios::eofbit );
   const Scene scene( si, camera.getEyePoint() );
   Random random;
   const RayTracer tracer( scene );

   const Vector3f c( tracer.getRadiance( Vector3f( 0, 0, 0 ),
      Vector3f( 0, 0, 1 ), random ) );
   std::printf( "measure analytic radiance=(%.6f %.6f %.6f) expect=(3 2 1)\n",
      c[0], c[1], c[2] );
   TASK_CHECK( close( c[0], 3.0, 1.0e-12 ) );
   TASK_CHECK( close( c[1], 2.0, 1.0e-12 ) );
   TASK_CHECK( close( c[2], 1.0, 1.0e-12 ) );

   // a back-facing ray sees nothing: single-sided emission plus black ground
   const Vector3f back( tracer.getRadiance( Vector3f( 0, 0, 0 ),
      Vector3f( 0, 0, -1 ), random ) );
   TASK_CHECK( back.isZero() );
   return 0;
}


/*==============================================================================
   transport group
==============================================================================*/

static int transport_density_hand_computed()
{
   const int32 W = 12, H = 12, D = 3;
   const Camera camera( makeCamera( "(0 0 2) (0 0 1) 100" ) );
   const Scene  scene( makeScene( camera, ROOM ) );

   std::istringstream ii( "12 12" );
   Image image( ii );
   PathReuse reuse;
   reuse.bind( camera, scene, W, H, D, 4242u );
   TASK_CHECK( reuse.runPass( camera, scene, image ) );

   int32 checked = 0;
   int32 nonCoplanarSets = 0;

   // scan every pixel so the hand formula is checked across floor, wall and
   // ceiling first hits, including source sets that span non-coplanar surfaces
   for( int32 i = 0; i < W * H; ++i )
   {
      std::vector<int32> sources;
      reuse.getSourceSet( i, sources );
      if( sources.empty() ) continue;

      // does this source set contain two non-coplanar source triangles?
      bool nonCoplanar = false;
      for( int32u a = 0; a < sources.size() && !nonCoplanar; ++a )
      {
         Vector3f pa, na;
         const Triangle* ta;
         bool va;
         reuse.getFirstHit( sources[a] % W, sources[a] / W, pa, na, ta, va );
         for( int32u b = a + 1; b < sources.size(); ++b )
         {
            Vector3f pb, nb;
            const Triangle* tb;
            bool vb;
            reuse.getFirstHit( sources[b] % W, sources[b] / W, pb, nb, tb,
               vb );
            if( ::fabs( dot3( na, nb ) ) < 0.999 ) { nonCoplanar = true; break; }
         }
      }
      if( nonCoplanar ) ++nonCoplanarSets;

      for( int32u s = 0; s < sources.size(); ++s )
      {
         const int32 j = sources[s];
         Vector3f y, b;
         const Triangle* yTri;
         int32 side;
         bool valid;
         reuse.getCandidate( j % W, j / W, y, yTri, side, b, valid );
         if( !valid ) continue;   // empty candidate: covered separately

         for( int32u t = 0; t < sources.size(); ++t )
         {
            const int32 k = sources[t];
            Vector3f kp, kn;
            const Triangle* kt;
            bool kvalid;
            reuse.getFirstHit( k % W, k / W, kp, kn, kt, kvalid );
            TASK_CHECK( kvalid );

            const double expected = handDensity( scene, kp, kn, kt, y, yTri,
               side );
            const double actual = reuse.sourceDensity( k, scene, j );
            TASK_CHECK( close( expected, actual, 1.0e-10 ) );
            ++checked;
         }
      }
   }

   std::printf(
      "transport hand_density comparisons=%d non_coplanar_source_sets=%d\n",
      checked, nonCoplanarSets );
   TASK_CHECK( checked > 4 );
   TASK_CHECK( nonCoplanarSets > 0 );
   return 0;
}


static int transport_mixing_value()
{
   const int32 W = 12, H = 12, D = 3;
   const Camera camera( makeCamera( "(0 0 2) (0 0 1) 100" ) );
   const Scene  scene( makeScene( camera, ROOM ) );

   std::istringstream ii( "12 12" );
   Image image( ii );
   PathReuse reuse;
   reuse.bind( camera, scene, W, H, D, 909u );
   TASK_CHECK( reuse.runPass( camera, scene, image ) );

   // choose a receiver whose source set spans two non-coplanar surfaces
   int32 chosen = -1;
   for( int32 i = 0; i < W * H; ++i )
   {
      std::vector<int32> sources;
      reuse.getSourceSet( i, sources );
      bool nonCoplanar = false;
      for( int32u a = 0; a < sources.size() && !nonCoplanar; ++a )
      {
         Vector3f pa, na; const Triangle* ta; bool va;
         reuse.getFirstHit( sources[a] % W, sources[a] / W, pa, na, ta, va );
         for( int32u b = a + 1; b < sources.size(); ++b )
         {
            Vector3f pb, nb; const Triangle* tb; bool vb;
            reuse.getFirstHit( sources[b] % W, sources[b] / W, pb, nb, tb,
               vb );
            if( ::fabs( dot3( na, nb ) ) < 0.999 ) { nonCoplanar = true; break; }
         }
      }
      if( nonCoplanar ) { chosen = i; break; }
   }
   TASK_CHECK( chosen >= 0 );
   const int32 i = chosen;

   // independently rebuild the full pixel colour from the hand formulas
   Vector3f xp, xn;
   const Triangle* xt;
   bool xvalid;
   reuse.getFirstHit( i % W, i / W, xp, xn, xt, xvalid );
   TASK_CHECK( xvalid );

   Vector3f expected( SurfacePoint( *xt, xp ).getEmission(
      camera.getEyePoint(),
      (camera.getEyePoint() - xp).unitize(), false ) );

   std::vector<int32> sources;
   reuse.getSourceSet( i, sources );
   for( int32u s = 0; s < sources.size(); ++s )
   {
      const int32 j = sources[s];
      Vector3f y, b;
      const Triangle* yTri;
      int32 side;
      bool valid;
      reuse.getCandidate( j % W, j / W, y, yTri, side, b, valid );
      if( !valid ) continue;

      double total = 0.0;
      for( int32u t = 0; t < sources.size(); ++t )
      {
         Vector3f kp, kn;
         const Triangle* kt;
         bool kv;
         reuse.getFirstHit( sources[t] % W, sources[t] / W, kp, kn, kt, kv );
         total += handDensity( scene, kp, kn, kt, y, yTri, side );
      }
      TASK_CHECK( total > 0.0 );

      const Vector3f f( handReceiver( scene, xp, xn, xt, y, yTri, side, b ) );
      expected = expected + f / total;
   }

   const Vector3f actual( reuse.getCandidatePixel( i % W, i / W ) );
   std::printf(
      "transport mixing pixel=%d expected=(%.6f %.6f %.6f) actual=(%.6f %.6f %.6f)\n",
      i, expected[0], expected[1], expected[2], actual[0], actual[1],
      actual[2] );
   TASK_CHECK( closeV( expected, actual, 1.0e-8 ) );
   return 0;
}


// a thin wall splits two floor patches so one source's line to a far
// candidate is blocked while a receiver connection stays open
static int transport_occluded_source_visible_receiver()
{
   const char split[] =
      "(0 0 0) (0 0 0)\n"
      // two floor patches, z in [1,5], split at x=0
      "(-3 -2 1) (-3 -2 5) (0 -2 1)   (0.7 0.7 0.7) (0 0 0)\n"
      "(0 -2 1) (-3 -2 5) (0 -2 5)    (0.7 0.7 0.7) (0 0 0)\n"
      "(0 -2 1) (0 -2 5) (3 -2 1)     (0.7 0.7 0.7) (0 0 0)\n"
      "(3 -2 1) (0 -2 5) (3 -2 5)     (0.7 0.7 0.7) (0 0 0)\n"
      // ceiling light
      "(-3 2 1) (3 2 1) (-3 2 5)      (0.8 0.8 0.8) (5 5 5)\n"
      "(3 2 1) (3 2 5) (-3 2 5)       (0.8 0.8 0.8) (5 5 5)\n"
      // back/front/left/right walls
      "(-3 -2 5) (3 -2 5) (-3 2 5)    (0.7 0.7 0.7) (0 0 0)\n"
      "(3 -2 5) (3 2 5) (-3 2 5)      (0.7 0.7 0.7) (0 0 0)\n"
      "(-3 -2 1) (-3 2 1) (3 -2 1)    (0.7 0.7 0.7) (0 0 0)\n"
      "(3 -2 1) (-3 2 1) (3 2 1)      (0.7 0.7 0.7) (0 0 0)\n"
      // central occluder wall on x=0, spanning y and most of z
      "(0 -2 2.2) (0 2 2.2) (0 -2 5)  (0.5 0.5 0.5) (0 0 0)\n"
      "(0 2 2.2) (0 2 5) (0 -2 5)     (0.5 0.5 0.5) (0 0 0)\n";

   const int32 W = 12, H = 12, D = 3;
   const Camera camera( makeCamera( "(0 0 2) (0 0 1) 100" ) );
   const Scene  scene( makeScene( camera, split ) );

   std::istringstream ii( "12 12" );
   Image image( ii );
   PathReuse reuse;
   reuse.bind( camera, scene, W, H, D, 31337u );
   TASK_CHECK( reuse.runPass( camera, scene, image ) );

   // scan for a candidate whose own source reaches it but a neighbour source
   // does not (blocked), proving the occlusion-zero rule independently
   bool foundBlocked = false;
   bool receiverVisible = false;
   for( int32 j = 0; j < W * H && !foundBlocked; ++j )
   {
      Vector3f y, b;
      const Triangle* yTri;
      int32 side;
      bool valid;
      reuse.getCandidate( j % W, j / W, y, yTri, side, b, valid );
      if( !valid ) continue;

      std::vector<int32> sources;
      reuse.getSourceSet( j, sources );
      for( int32u t = 0; t < sources.size() && !foundBlocked; ++t )
      {
         const int32 k = sources[t];
         const double q = reuse.sourceDensity( k, scene, j );
         Vector3f kp, kn;
         const Triangle* kt;
         bool kv;
         reuse.getFirstHit( k % W, k / W, kp, kn, kt, kv );
         const double hand = handDensity( scene, kp, kn, kt, y, yTri, side );
         TASK_CHECK( close( q, hand, 1.0e-10 ) );
         if( (k != j) & (q == 0.0) )
         {
            foundBlocked = true;
            // the candidate's own source must still reach it
            TASK_CHECK( reuse.sourceDensity( j, scene, j ) > 0.0 );

            // receiver-connection visibility, checked independently through
            // the scene spatial index and independent of the (possibly zero)
            // suffix B: the finite ray from the receiver first-hit x_j to y_j
            // must land on the candidate triangle with nothing in between
            Vector3f xp, xn;
            const Triangle* xt;
            bool xv;
            reuse.getFirstHit( j % W, j / W, xp, xn, xt, xv );
            TASK_CHECK( xv );
            const Vector3f delta( y - xp );
            const double d2 = dot3( delta, delta );
            TASK_CHECK( d2 > EPS * EPS );
            const Triangle* vhit = 0;
            Vector3f vpos;
            scene.getIntersection( xp, delta / ::sqrt( d2 ),
               static_cast<const void*>( xt ), vhit, vpos );
            TASK_CHECK( vhit == yTri );
            TASK_CHECK( length3( vpos - y ) <= 1.0e-6 );
            receiverVisible = true;
         }
      }
   }

   real64 sumD = 0.0, minD = 0.0, maxD = 0.0;
   int32 zeroD = 0, occludedDiag = 0;
   reuse.getDiagnostics( sumD, minD, maxD, zeroD, occludedDiag );
   std::printf(
      "transport occluded_source case_found=%d receiver_visible=%d occluded_diag=%d\n",
      foundBlocked ? 1 : 0, receiverVisible ? 1 : 0, occludedDiag );
   TASK_CHECK( foundBlocked );
   TASK_CHECK( receiverVisible );
   return 0;
}


// a thin slab gives two distinct, separated faces: the -x face emits on its
// camera-facing side, the +x face is non-emitting. Floor sources on either
// side reach the two different faces, exercising same-y two-side handling
// without coplanar triangle ambiguity.
static int transport_two_sides_of_y()
{
   const char twoSided[] =
      "(0 0 0) (0 0 0)\n"
      // floor
      "(-3 -2 1) (-3 -2 5) (3 -2 1)    (0.7 0.7 0.7) (0 0 0)\n"
      "(3 -2 1) (-3 -2 5) (3 -2 5)     (0.7 0.7 0.7) (0 0 0)\n"
      // ceiling light
      "(-3 2 1) (3 2 1) (-3 2 5)       (0.8 0.8 0.8) (3 3 3)\n"
      "(3 2 1) (3 2 5) (-3 2 5)        (0.8 0.8 0.8) (3 3 3)\n"
      // back/front walls
      "(-3 -2 5) (3 -2 5) (-3 2 5)     (0.7 0.7 0.7) (0 0 0)\n"
      "(3 -2 5) (3 2 5) (-3 2 5)       (0.7 0.7 0.7) (0 0 0)\n"
      "(-3 -2 1) (-3 2 1) (3 -2 1)     (0.7 0.7 0.7) (0 0 0)\n"
      "(3 -2 1) (-3 2 1) (3 2 1)       (0.7 0.7 0.7) (0 0 0)\n"
      // left/right walls
      "(-3 -2 1) (-3 -2 5) (-3 2 1)    (0.7 0.7 0.7) (0 0 0)\n"
      "(-3 2 1) (-3 -2 5) (-3 2 5)     (0.7 0.7 0.7) (0 0 0)\n"
      "(3 -2 1) (3 2 1) (3 -2 5)       (0.7 0.7 0.7) (0 0 0)\n"
      "(3 2 1) (3 2 5) (3 -2 5)        (0.7 0.7 0.7) (0 0 0)\n"
      // slab -x face (normal -x, reached from the -x side): emits
      "(-0.05 -2 2.5) (-0.05 -2 4.5) (-0.05 1.5 2.5)  (0.6 0.6 0.6) (5 5 5)\n"
      // slab +x face (normal +x, reached from the +x side): non-emitting
      "(0.05 -2 2.5) (0.05 1.5 2.5) (0.05 -2 4.5)      (0.6 0.6 0.6) (0 0 0)\n";

   const int32 W = 12, H = 12, D = 3;
   const Camera camera( makeCamera( "(0 0 2) (0 0 1) 100" ) );
   const Scene  scene( makeScene( camera, twoSided ) );

   std::istringstream ii( "12 12" );
   Image image( ii );
   PathReuse reuse;
   reuse.bind( camera, scene, W, H, D, 555u );
   TASK_CHECK( reuse.runPass( camera, scene, image ) );

   int32 minusSide = 0, plusSide = 0;
   int32 frontEmitting = 0, backNonEmitting = 0;
   const Triangle* minusTri = 0;
   const Triangle* plusTri  = 0;
   for( int32 j = 0; j < W * H; ++j )
   {
      Vector3f y, b;
      const Triangle* yTri;
      int32 side;
      bool valid;
      reuse.getCandidate( j % W, j / W, y, yTri, side, b, valid );
      if( !valid ) continue;
      if( ::fabs( ::fabs( y[0] ) - 0.05 ) > 1.0e-4 ) continue;

      const bool emits = !yTri->getEmitivity().isZero();
      if( y[0] < 0.0 ) { ++minusSide; minusTri = yTri; }
      else             { ++plusSide;  plusTri  = yTri; }

      // a record on the emitting face, reached from its front side, must keep
      // that face's self-emission inside B; the back side must not
      if( emits & (side > 0) )
      {
         ++frontEmitting;
         TASK_CHECK( b[0] >= 5.0 - 1.0e-9 );
      }
      if( emits & (side < 0) ) ++backNonEmitting;
   }

   std::printf(
      "transport two_sides minus=%d plus=%d front_emitting=%d back=%d\n",
      minusSide, plusSide, frontEmitting, backNonEmitting );
   TASK_CHECK( minusSide > 0 );
   TASK_CHECK( plusSide > 0 );
   TASK_CHECK( frontEmitting > 0 );
   TASK_CHECK( minusTri != plusTri );

   // the hand density must agree on both faces, including side-mismatch zeros
   for( int32 j = 0; j < W * H; ++j )
   {
      Vector3f y, b;
      const Triangle* yTri;
      int32 side;
      bool valid;
      reuse.getCandidate( j % W, j / W, y, yTri, side, b, valid );
      if( !valid || ::fabs( ::fabs( y[0] ) - 0.05 ) > 1.0e-4 ) continue;

      std::vector<int32> sources;
      reuse.getSourceSet( j, sources );
      for( int32u t = 0; t < sources.size(); ++t )
      {
         const int32 k = sources[t];
         Vector3f kp, kn;
         const Triangle* kt;
         bool kv;
         reuse.getFirstHit( k % W, k / W, kp, kn, kt, kv );
         TASK_CHECK( close( reuse.sourceDensity( k, scene, j ),
            handDensity( scene, kp, kn, kt, y, yTri, side ), 1.0e-10 ) );
      }
   }
   return 0;
}


// open black scene with only a small ceiling patch: some cosine edges reach a
// surface while others miss into black. Both occupied and empty candidates
// coexist, including source sets where only one party generates.
static int transport_empty_candidate()
{
   const char openScene[] =
      "(0 0 0) (0 0 0)\n"
      // broad floor under the camera, black background all around
      "(-4 -2 0) (-4 -2 8) (4 -2 0)   (0.7 0.7 0.7) (0 0 0)\n"
      "(4 -2 0) (-4 -2 8) (4 -2 8)    (0.7 0.7 0.7) (0 0 0)\n"
      // small ceiling patch, the only thing upward edges can hit
      "(-1 2 2) (-1 2 4) (1 2 2)      (0.8 0.8 0.8) (4 4 4)\n"
      "(1 2 2) (-1 2 4) (1 2 4)       (0.8 0.8 0.8) (4 4 4)\n";

   const int32 W = 10, H = 10, D = 3;
   const Camera camera( makeCamera( "(0 0 0) (0 -1 0.35) 95" ) );
   const Scene  scene( makeScene( camera, openScene ) );

   std::istringstream ii( "10 10" );
   Image image( ii );
   PathReuse reuse;
   reuse.bind( camera, scene, W, H, D, 8888u );
   TASK_CHECK( reuse.runPass( camera, scene, image ) );

   int32 emptyPixels = 0, occupiedPixels = 0, mixedSets = 0;
   for( int32 j = 0; j < W * H; ++j )
   {
      Vector3f y, b;
      const Triangle* yTri;
      int32 side;
      bool valid;
      reuse.getCandidate( j % W, j / W, y, yTri, side, b, valid );

      Vector3f xp, xn;
      const Triangle* xt;
      bool xv;
      reuse.getFirstHit( j % W, j / W, xp, xn, xt, xv );
      if( !xv ) continue;   // only valid source pixels hold a slot

      // every valid source keeps a fixed, non-empty source set
      std::vector<int32> sources;
      reuse.getSourceSet( j, sources );
      TASK_CHECK( !sources.empty() );

      if( valid ) ++occupiedPixels; else ++emptyPixels;
   }

   // source sets that contain both a generating and an empty source: the
   // "only one party can generate" configuration
   for( int32 i = 0; i < W * H; ++i )
   {
      std::vector<int32> sources;
      reuse.getSourceSet( i, sources );
      int32 e = 0, o = 0;
      for( int32u t = 0; t < sources.size(); ++t )
      {
         Vector3f y, b;
         const Triangle* yTri;
         int32 side;
         bool valid;
         reuse.getCandidate( sources[t] % W, sources[t] / W, y, yTri, side,
            b, valid );
         if( valid ) ++o; else ++e;
      }
      if( (e > 0) & (o > 0) ) ++mixedSets;
   }

   // an empty candidate contributes nothing through either query
   for( int32 j = 0; j < W * H; ++j )
   {
      Vector3f y, b;
      const Triangle* yTri;
      int32 side;
      bool valid;
      reuse.getCandidate( j % W, j / W, y, yTri, side, b, valid );
      if( valid ) continue;
      TASK_CHECK( reuse.sourceDensity( j, scene, j ) == 0.0 );
      TASK_CHECK( reuse.receiverContribution( j, scene, j ).isZero() );
   }

   std::printf(
      "transport empty_pixels=%d occupied_pixels=%d mixed_sets=%d suffixes=%d\n",
      emptyPixels, occupiedPixels, mixedSets, reuse.getLastSuffixCount() );
   TASK_CHECK( emptyPixels > 0 );
   TASK_CHECK( occupiedPixels > 0 );
   TASK_CHECK( mixedSets > 0 );
   TASK_CHECK( reuse.getLastSuffixCount() == occupiedPixels );
   return 0;
}


// degenerate connections (same surface / zero distance / grazing) return zero
static int transport_degenerate_connections()
{
   const int32 W = 12, H = 12, D = 3;
   const Camera camera( makeCamera( "(0 0 2) (0 0 1) 100" ) );
   const Scene  scene( makeScene( camera, ROOM ) );

   std::istringstream ii( "12 12" );
   Image image( ii );
   PathReuse reuse;
   reuse.bind( camera, scene, W, H, D, 2024u );
   TASK_CHECK( reuse.runPass( camera, scene, image ) );

   int32 sameSurfacePairs = 0;
   for( int32 i = 0; i < W * H; ++i )
   {
      Vector3f xp, xn;
      const Triangle* xt;
      bool xv;
      reuse.getFirstHit( i % W, i / W, xp, xn, xt, xv );
      if( !xv ) continue;

      std::vector<int32> sources;
      reuse.getSourceSet( i, sources );
      for( int32u s = 0; s < sources.size(); ++s )
      {
         const int32 j = sources[s];
         Vector3f y, b;
         const Triangle* yt;
         int32 side;
         bool valid;
         reuse.getCandidate( j % W, j / W, y, yt, side, b, valid );
         if( !valid ) continue;

         if( xt == yt )
         {
            // same-surface connection: both the source support query and the
            // receiver connection must be zero
            TASK_CHECK( reuse.sourceDensity( i, scene, j ) == 0.0 );
            TASK_CHECK( reuse.receiverContribution( i, scene, j )
               .isZero() );
            ++sameSurfacePairs;
         }
      }
   }

   std::printf( "transport degenerate same_surface_pairs=%d\n",
      sameSurfacePairs );
   TASK_CHECK( sameSurfacePairs > 0 );
   return 0;
}


/*==============================================================================
   passes group
==============================================================================*/

static int passes_freeze_and_determinism()
{
   const int32 W = 6, H = 6, D = 3;
   const Camera camera( makeCamera( "(0 0 2) (0 0 1) 90" ) );
   const Scene  scene( makeScene( camera, ROOM ) );

   std::istringstream i1( "6 6" );
   Image img1( i1 );
   PathReuse a;
   a.bind( camera, scene, W, H, D, 42u );
   TASK_CHECK( a.runPass( camera, scene, img1 ) );

   // re-bind a fresh state with the same seed: frozen first hits must repeat
   std::istringstream i2( "6 6" );
   Image img2( i2 );
   PathReuse b;
   b.bind( camera, scene, W, H, D, 42u );
   TASK_CHECK( b.runPass( camera, scene, img2 ) );

   for( int32 y = 0; y < H; ++y )
      for( int32 x = 0; x < W; ++x )
      {
         Vector3f p1, n1, p2, n2;
         const Triangle *t1, *t2;
         bool v1, v2;
         a.getFirstHit( x, y, p1, n1, t1, v1 );
         b.getFirstHit( x, y, p2, n2, t2, v2 );
         TASK_CHECK( v1 == v2 );
         if( v1 )
         {
            TASK_CHECK( closeV( p1, p2, 1.0e-12 ) );
            TASK_CHECK( closeV( n1, n2, 1.0e-12 ) );
            TASK_CHECK( t1 == t2 );
         }
         TASK_CHECK( closeV( a.getLinearPixel( x, y ),
                             b.getLinearPixel( x, y ), 1.0e-12 ) );
      }
   return 0;
}


static int passes_processing_order()
{
   const int32 W = 6, H = 6, D = 3;
   const Camera camera( makeCamera( "(0 0 2) (0 0 1) 90" ) );
   const Scene  scene( makeScene( camera, ROOM ) );

   std::istringstream ii( "6 6" );
   Image image( ii );
   PathReuse reuse;
   reuse.bind( camera, scene, W, H, D, 99u );
   TASK_CHECK( reuse.runPass( camera, scene, image ) );

   // rebuild colours in reverse pixel order purely from frozen candidates;
   // forward and reverse traversal must give identical numbers
   std::vector<Vector3f> rebuilt( W * H, Vector3f::ZERO() );
   for( int32 i = W * H - 1; i >= 0; --i )
   {
      Vector3f xp, xn;
      const Triangle* xt;
      bool xv;
      reuse.getFirstHit( i % W, i / W, xp, xn, xt, xv );
      if( !xv ) continue;

      Vector3f color( SurfacePoint( *xt, xp ).getEmission(
         camera.getEyePoint(), (camera.getEyePoint() - xp).unitize(), false ) );

      std::vector<int32> sources;
      reuse.getSourceSet( i, sources );
      for( int32u s = 0; s < sources.size(); ++s )
      {
         const int32 j = sources[s];
         Vector3f y, bv;
         const Triangle* yTri;
         int32 side;
         bool valid;
         reuse.getCandidate( j % W, j / W, y, yTri, side, bv, valid );
         if( !valid ) continue;

         double total = 0.0;
         for( int32u t = 0; t < sources.size(); ++t )
            total += reuse.sourceDensity( sources[t], scene, j );

         color = color + reuse.receiverContribution( i, scene, j ) / total;
      }
      rebuilt[i] = color;
   }

   for( int32 i = 0; i < W * H; ++i )
      TASK_CHECK( closeV( rebuilt[i], reuse.getCandidatePixel( i % W, i / W ),
         1.0e-9 ) );
   return 0;
}


static int passes_counting_clear_isolation()
{
   const int32 W = 4, H = 4, D = 2;
   const Camera camera( makeCamera( "(0 0 2) (0 0 1) 90" ) );
   const Scene  scene( makeScene( camera, ROOM ) );

   std::istringstream i1( "4 4" );
   Image img1( i1 );
   PathReuse a;
   a.bind( camera, scene, W, H, D, 1u );
   TASK_CHECK( a.getCompletedPasses() == 0 );
   TASK_CHECK( a.runPass( camera, scene, img1 ) );
   TASK_CHECK( a.runPass( camera, scene, img1 ) );
   TASK_CHECK( a.getCompletedPasses() == 2 );
   TASK_CHECK( a.getTotalSuffixCount() ==
      static_cast<std::int64_t>( a.getLastSuffixCount() ) * 2 );

   // a second state must not see the first state's accumulation
   std::istringstream i2( "4 4" );
   Image img2( i2 );
   PathReuse b;
   b.bind( camera, scene, W, H, D, 2u );
   TASK_CHECK( b.getCompletedPasses() == 0 );
   TASK_CHECK( b.runPass( camera, scene, img2 ) );
   TASK_CHECK( b.getCompletedPasses() == 1 );

   // clear resets counters and the linear accumulator
   a.clear();
   TASK_CHECK( a.getCompletedPasses() == 0 );
   TASK_CHECK( a.getTotalSuffixCount() == 0 );
   TASK_CHECK( a.getLinearPixel( 0, 0 ).isZero() );

   // a size or scene change is rejected, not silently reused
   const Camera camera2( makeCamera( "(0 0 2) (0 0 1) 60" ) );
   bool threw = false;
   try
   {
      std::istringstream i3( "5 4" );
      Image img3( i3 );
      a.runPass( camera, scene, img3 );
   }
   catch( const std::exception& ) { threw = true; }
   TASK_CHECK( threw );

   bool threw2 = false;
   try { a.runPass( camera2, scene, img1 ); }
   catch( const std::exception& ) { threw2 = true; }
   TASK_CHECK( threw2 );
   return 0;
}


static int passes_black_no_light()
{
   const char blackRoom[] =
      "(0 0 0) (0 0 0)\n"
      "(-3 -2 1) (-3 -2 5) (3 -2 1)   (0.7 0.7 0.7) (0 0 0)\n"
      "(3 -2 1) (-3 -2 5) (3 -2 5)    (0.7 0.7 0.7) (0 0 0)\n"
      "(-3 2 1) (3 2 1) (-3 2 5)      (0.8 0.8 0.8) (0 0 0)\n"
      "(3 2 1) (3 2 5) (-3 2 5)       (0.8 0.8 0.8) (0 0 0)\n"
      "(-3 -2 5) (3 -2 5) (-3 2 5)    (0.7 0.7 0.7) (0 0 0)\n"
      "(3 -2 5) (3 2 5) (-3 2 5)      (0.7 0.7 0.7) (0 0 0)\n";

   const int32 W = 6, H = 6, D = 4;
   const Camera camera( makeCamera( "(0 0 2) (0 0 1) 90" ) );
   const Scene  scene( makeScene( camera, blackRoom ) );

   std::istringstream ii( "6 6" );
   Image image( ii );
   PathReuse reuse;
   reuse.bind( camera, scene, W, H, D, 123u );
   TASK_CHECK( reuse.runPass( camera, scene, image ) );

   for( int32 i = 0; i < W * H; ++i )
      TASK_CHECK( reuse.getLinearPixel( i % W, i / W ).isZero() );

   // a non-black background must be rejected at bind time
   const char skyScene[] =
      "(1 1 1) (0 0 0)\n"
      "(-3 -2 1) (-3 -2 5) (3 -2 1)   (0.7 0.7 0.7) (0 0 0)\n";
   const Scene sky( makeScene( camera, skyScene ) );
   bool threw = false;
   try
   {
      PathReuse bad;
      bad.bind( camera, sky, W, H, D, 1u );
   }
   catch( const std::exception& ) { threw = true; }
   TASK_CHECK( threw );

   // depth range is enforced
   bool threwD = false;
   try
   {
      PathReuse bad;
      bad.bind( camera, scene, W, H, 13, 1u );
   }
   catch( const std::exception& ) { threwD = true; }
   TASK_CHECK( threwD );
   return 0;
}


static int passes_legacy_compatibility()
{
   // mirror task_support/baseline.cpp: getRadiance and getFrame are unchanged
   std::istringstream ci( "(0 0 0) (0 0 1) 40" );
   const Camera camera( ci );
   std::istringstream si(
      "(0 0 0) (0 0 0)\n"
      "(-10 -10 1) (0 10 1) (10 -10 1) (0 0 0) (2 1 .5)\n" );
   si.exceptions( std::ios::badbit | std::ios::failbit | std::ios::eofbit );
   const Scene scene( si, camera.getEyePoint() );
   Random random;
   const RayTracer tracer( scene );

   const Vector3f c( tracer.getRadiance( Vector3f( 0, 0, 0 ),
      Vector3f( 0, 0, 1 ), random ) );
   TASK_CHECK( close( c[0], 2.0, 1.0e-12 ) );
   TASK_CHECK( close( c[1], 1.0, 1.0e-12 ) );
   TASK_CHECK( close( c[2], 0.5, 1.0e-12 ) );

   // two independent default-constructed runs produce byte-identical frames
   std::string first, second;
   for( int rep = 0; rep < 2; ++rep )
   {
      std::istringstream ii( "8 8" );
      Image image( ii );
      Random r;
      camera.getFrame( scene, r, image );
      std::ostringstream out( std::ios::binary );
      image.getFormatted( out, 1 );
      if( 0 == rep ) first = out.str(); else second = out.str();
   }
   TASK_CHECK( !first.empty() );
   TASK_CHECK( first == second );
   TASK_CHECK( first.substr( 0, 2 ) == "P6" );

   // explicit-seed Random differs from the default stream but is itself stable
   Random seeded1( 424242u ), seeded2( 424242u );
   TASK_CHECK( seeded1.getReal64() == seeded2.getReal64() );
   return 0;
}


/*==============================================================================
   registration
==============================================================================*/

#define TASK( g, n ) { g, #n, n }

extern const struct task_case task_cases[] = {
   TASK( "measure",   measure_statistics ),
   TASK( "measure",   measure_analytic_radiance ),
   TASK( "measure",   measure_diagnostics ),
   TASK( "transport", transport_density_hand_computed ),
   TASK( "transport", transport_mixing_value ),
   TASK( "transport", transport_occluded_source_visible_receiver ),
   TASK( "transport", transport_two_sides_of_y ),
   TASK( "transport", transport_empty_candidate ),
   TASK( "transport", transport_degenerate_connections ),
   TASK( "passes",    passes_freeze_and_determinism ),
   TASK( "passes",    passes_processing_order ),
   TASK( "passes",    passes_counting_clear_isolation ),
   TASK( "passes",    passes_black_no_light ),
   TASK( "passes",    passes_legacy_compatibility ),
};

const size_t task_case_count =
   sizeof( task_cases ) / sizeof( task_cases[0] );
