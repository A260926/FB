/*------------------------------------------------------------------------------

   MiniLight C++ : path guiding tests

   Registered groups: density, transport, passes.

------------------------------------------------------------------------------*/


#include "check.hpp"

#include <cmath>
#include <cstdio>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "Camera.hpp"
#include "Image.hpp"
#include "PathGuide.hpp"
#include "Random.hpp"
#include "RayTracer.hpp"
#include "Scene.hpp"
#include "SurfacePoint.hpp"
#include "Triangle.hpp"
#include "Vector3f.hpp"


using namespace hxa7241_minilight;
using namespace hxa7241_general;


namespace
{
   const real64 PI = 3.14159265358979;


   Scene* newScene( const std::string& body )
   {
      std::istringstream in( body );
      in.exceptions( std::ios::badbit | std::ios::eofbit | std::ios::failbit );
      return new Scene( in, Vector3f( 0.0, 1.0, 0.0 ) );
   }


   bool nearVec( const Vector3f& a, const Vector3f& b, const real64 eps )
   {
      for( int c = 0;  c < 3;  ++c )
      {
         if( std::fabs( a[c] - b[c] ) > eps )
         {
            return false;
         }
      }
      return true;
   }


   // direction from local spherical coordinates; only used with the density
   // scene triangle whose front-side local frame equals the world axes
   Vector3f dirFromLocal( const real64 phi, const real64 z )
   {
      const real64 r = std::sqrt( 1.0 - (z * z) );
      return Vector3f( std::cos( phi ) * r, std::sin( phi ) * r, z );
   }


   // world direction that maps to local (phi, z) on the back side of the
   // density scene triangle (frame mirrored by the flipped normal)
   Vector3f dirFromLocalBack( const real64 phi, const real64 z )
   {
      const real64 r = std::sqrt( 1.0 - (z * z) );
      return Vector3f( std::cos( phi ) * r, -std::sin( phi ) * r, -z );
   }


   // numerical integral of a guide density over the hemisphere, with a
   // midpoint grid aligned to the bin layout (local == world frame here)
   real64 integrateHemisphere( const PathGuide& guide,
                               const Triangle&  triangle,
                               const Vector3f&  inDirection )
   {
      const int NP = 240;
      const int NZ = 120;
      real64 sum = 0.0;
      for( int i = 0;  i < NP;  ++i )
      {
         const real64 phi = (i + 0.5) * (2.0 * PI / NP);
         for( int j = 0;  j < NZ;  ++j )
         {
            const real64 z = (j + 0.5) / NZ;
            sum += guide.getPdf( triangle, inDirection,
               dirFromLocal( phi, z ) );
         }
      }
      return sum * (2.0 * PI / NP) * (1.0 / NZ);
   }
}




/// density --------------------------------------------------------------------

CASE(density)
{
   std::unique_ptr<Scene> pScene( newScene(
      "(0 0 0) (0 0 0)\n"
      "(0 0 0) (1 0 0) (0 1 0) (0.5 0.5 0.5) (0 0 0)\n" ) );
   const Triangle& triangle = pScene->getTriangles()[0];
   PathGuide guide( *pScene );

   const Vector3f inFront( 0.0, 0.0, 1.0 );
   const Vector3f inBack ( 0.0, 0.0, -1.0 );

   // bin layout in the local frame (tangent +x, bitangent +y, normal +z):
   // bin = zBand * 8 + azimuthBand; directions at bin centers
   CHECK( guide.getBinIndex( triangle, inFront, Vector3f( 0.0, 0.0, 1.0 ) )
      == 24 );
   CHECK( guide.getBinIndex( triangle, inFront,
      dirFromLocal( PI/8, 0.5 ) ) == 16 );
   CHECK( guide.getBinIndex( triangle, inFront,
      dirFromLocal( 5*PI/8, 0.5 ) ) == 18 );
   CHECK( guide.getBinIndex( triangle, inFront,
      dirFromLocal( 9*PI/8, 0.5 ) ) == 20 );
   CHECK( guide.getBinIndex( triangle, inFront,
      dirFromLocal( 13*PI/8, 0.5 ) ) == 22 );
   // below the horizon has no bin and zero density
   CHECK( guide.getBinIndex( triangle, inFront, Vector3f( 0.0, 0.0, -1.0 ) )
      == -1 );
   CHECK( guide.getPdf( triangle, inFront, Vector3f( 0.0, 0.0, -1.0 ) )
      == 0.0 );

   // bin boundaries: azimuth splits at k*pi/4, z splits at k*0.25
   const real64 e = 1e-6;
   CHECK( guide.getBinIndex( triangle, inFront,
      dirFromLocal( PI/4 - e, 0.5 ) ) == 16 );
   CHECK( guide.getBinIndex( triangle, inFront,
      dirFromLocal( PI/4 + e, 0.5 ) ) == 17 );
   CHECK( guide.getBinIndex( triangle, inFront,
      dirFromLocal( 3*PI/4 - e, 0.5 ) ) == 18 );
   CHECK( guide.getBinIndex( triangle, inFront,
      dirFromLocal( 3*PI/4 + e, 0.5 ) ) == 19 );
   CHECK( guide.getBinIndex( triangle, inFront,
      dirFromLocal( PI/8, 0.25 - e ) ) == 0 );
   CHECK( guide.getBinIndex( triangle, inFront,
      dirFromLocal( PI/8, 0.25 ) ) == 8 );
   CHECK( guide.getBinIndex( triangle, inFront,
      dirFromLocal( PI/8, 0.75 - e ) ) == 16 );
   CHECK( guide.getBinIndex( triangle, inFront,
      dirFromLocal( PI/8, 0.75 ) ) == 24 );

   // back side: same triangle, frame mirrored by the flipped normal
   CHECK( guide.getBinIndex( triangle, inBack, Vector3f( 0.0, 0.0, -1.0 ) )
      == 24 );
   CHECK( guide.getBinIndex( triangle, inBack,
      dirFromLocalBack( PI/8, 0.5 ) ) == 16 );
   CHECK( guide.getBinIndex( triangle, inBack,
      dirFromLocalBack( 5*PI/8, 0.5 ) ) == 18 );
   CHECK( guide.getBinIndex( triangle, inBack,
      dirFromLocalBack( 9*PI/8, 0.5 ) ) == 20 );
   CHECK( guide.getBinIndex( triangle, inBack,
      dirFromLocalBack( 13*PI/8, 0.5 ) ) == 22 );
   CHECK( guide.getBinIndex( triangle, inBack, Vector3f( 0.0, 0.0, 1.0 ) )
      == -1 );

   guide.beginPass();

   // cold mixture (half cosine, half uniform) integrates to one
   CHECK( std::fabs( integrateHemisphere( guide, triangle, inFront ) - 1.0 )
      < 1e-9 );

   // cold sampling: returned pdf equals queried pdf, directions are unit
   // length on the hemisphere, and bin frequencies follow the mixture
   {
      Random random( 12345 );
      const int N = 200000;
      std::vector<int> counts( 32, 0 );
      for( int i = 0;  i < N;  ++i )
      {
         Vector3f direction;
         const real64 pdf = guide.sampleDirection( triangle, inFront, random,
            direction );
         CHECK( pdf > 0.0 );
         CHECK( std::fabs( pdf - guide.getPdf( triangle, inFront, direction ) )
            < 1e-12 );
         CHECK( std::fabs( std::sqrt( direction.dot( direction ) ) - 1.0 )
            < 1e-9 );
         CHECK( direction[2] >= 0.0 );
         const int bin = guide.getBinIndex( triangle, inFront, direction );
         CHECK( (bin >= 0) & (bin < 32) );
         ++counts[ bin ];
      }
      // expected: 0.5 * cosine integral over the bin + 0.5 * 1/32
      real64 chi2 = 0.0;
      for( int b = 0;  b < 32;  ++b )
      {
         const real64 cosinePart = (2.0 * (b / 8) + 1.0) / 128.0;
         const real64 expected   = N * ((0.5 * cosinePart) + (0.5 / 32.0));
         const real64 diff       = counts[ b ] - expected;
         chi2 += (diff * diff) / expected;
      }
      CHECK( chi2 < 75.0 );
   }

   // train one bin (pole direction, bin 24) with a real mass
   guide.train( triangle, inFront, Vector3f( 0.0, 0.0, 1.0 ), 50.0 );
   CHECK( guide.getTrainingCount() == 1 );
   // not visible before the pass is finished
   CHECK( guide.getBinMass( triangle, inFront, 24 ) == 0.0 );
   guide.finishPass();
   CHECK( guide.getBinMass( triangle, inFront, 24 ) == 50.0 );
   // the other side of the same triangle keeps its own (zero) state
   CHECK( guide.getBinMass( triangle, inBack, 24 ) == 0.0 );

   // the learned distribution still integrates to one
   CHECK( std::fabs( integrateHemisphere( guide, triangle, inFront ) - 1.0 )
      < 1e-9 );

   // the trained bin is much denser than an untrained one
   const real64 pdfHot  = guide.getPdf( triangle, inFront,
      Vector3f( 0.0, 0.0, 1.0 ) );
   const real64 pdfRim  = guide.getPdf( triangle, inFront,
      Vector3f( 1.0, 0.0, 0.0 ) );
   CHECK( pdfHot > 2.0 );
   CHECK( pdfHot > 100.0 * pdfRim );

   // and it is sampled much more often, while untrained bins drop to the
   // small uniform floor
   {
      Random random( 54321 );
      const int N = 40000;
      int hot = 0;
      int rim = 0;
      for( int i = 0;  i < N;  ++i )
      {
         Vector3f direction;
         const real64 pdf = guide.sampleDirection( triangle, inFront, random,
            direction );
         CHECK( pdf > 0.0 );
         CHECK( std::fabs( pdf - guide.getPdf( triangle, inFront, direction ) )
            < 1e-12 );
         CHECK( direction[2] >= 0.0 );
         const int bin = guide.getBinIndex( triangle, inFront, direction );
         hot += (bin == 24) ? 1 : 0;
         rim += (bin == 0)  ? 1 : 0;
      }
      // cold expectation for bin 24 is ~4.3%; the trained bin holds nearly
      // all of the guide half
      CHECK( hot > N / 4 );
      // cold expectation for bin 0 is ~2%; now only the floor remains
      CHECK( rim < N / 100 );
   }
}




/// transport ------------------------------------------------------------------

CASE(transport)
{
   // diffuse expectation under a uniform sky, with guided continuation:
   // E = reflectivity * skyEmission (same value as the original estimator)
   {
      std::unique_ptr<Scene> pScene( newScene(
         "(1 2 4) (1 1 1)\n"
         "(-10 0 -10) (-10 0 10) (10 0 -10) (.4 .5 .6) (0 0 0)\n" ) );
      PathGuide guide( *pScene );
      RayTracer tracer( *pScene, &guide );
      Random random( 777 );
      guide.beginPass();
      Vector3f sum;
      for( int i = 0;  i < 20000;  ++i )
      {
         sum = sum + tracer.getRadiance( Vector3f( -1.0, 1.0, -1.0 ),
            Vector3f( 0.0, -1.0, 0.0 ), random );
      }
      guide.finishPass();
      const Vector3f mean( sum / 20000.0 );
      CHECK( nearVec( mean, Vector3f( 0.4, 1.0, 2.4 ), 0.035 ) );
      CHECK( guide.getTrainingCount() > 0 );
   }

   // finite area light that can be integrated independently:
   // emitter at y=1 emitting downward, diffuse receiver at y=0
   const Vector3f e0( -2.0, 1.0, -2.0 );
   const Vector3f e1(  2.0, 1.0, -2.0 );
   const Vector3f e2( -2.0, 1.0,  2.0 );
   const Vector3f eNormal( 0.0, -1.0, 0.0 );
   const real64   eArea = 8.0;
   const Vector3f emit( 2.0, 4.0, 6.0 );
   const Vector3f rNormal( 0.0, 1.0, 0.0 );
   const Vector3f p( -1.0, 0.0, -1.0 );
   const real64   rho = 0.5;

   const std::string emitterLine(
      "(-2 1 -2) (2 1 -2) (-2 1 2) (0 0 0) (2 4 6)\n" );
   const std::string emitterBackLine(
      "(-2 1 -2) (-2 1 2) (2 1 -2) (0 0 0) (2 4 6)\n" );
   const std::string receiverLine(
      "(-4 0 -4) (-4 0 4) (4 0 -4) (0.5 0.5 0.5) (0 0 0)\n" );

   // independent numerical integration over the emitter area:
   // total L = rho/pi * Le * ∫ cosRec cosLight / d^2 dA, and the
   // emitter-sampling-only part with the balance-heuristic weight computed
   // from the actual (cold) continuation density
   real64 intTotal = 0.0;
   real64 intNee   = 0.0;
   {
      std::unique_ptr<Scene> pScene( newScene(
         "(0 0 0) (0 0 0)\n" + emitterLine + receiverLine ) );
      PathGuide guide( *pScene );
      const Triangle& receiver = pScene->getTriangles()[1];
      const int M = 160;
      real64 sumTotal = 0.0;
      real64 sumNee   = 0.0;
      int    count    = 0;
      for( int i = 0;  i < M;  ++i )
      {
         for( int j = 0;  j < M;  ++j )
         {
            const real64 u = (i + 0.5) / M;
            const real64 v = (j + 0.5) / M;
            if( u + v >= 1.0 )
            {
               continue;
            }
            ++count;
            const Vector3f y( (e0 * (1.0 - u - v)) + (e1 * u) + (e2 * v) );
            Vector3f w( y - p );
            const real64 d2 = w.dot( w );
            w = w / std::sqrt( d2 );
            const real64 cosRec   =  w.dot( rNormal );
            const real64 cosLight = -w.dot( eNormal );
            if( (cosRec <= 0.0) | (cosLight <= 0.0) )
            {
               continue;
            }
            const real64 g = (cosRec * cosLight) / d2;
            sumTotal += g;
            // balance-heuristic weight of the emitter-sample connection
            const real64 pLight = d2 / (1.0 * eArea * cosLight);
            const real64 q      = guide.getPdf( receiver, rNormal, w );
            sumNee += g * (pLight / (pLight + q));
         }
      }
      intTotal = (sumTotal / count) * eArea;
      intNee   = (sumNee   / count) * eArea;
   }
   CHECK( intTotal > 0.0 );
   CHECK( intNee < intTotal );

   // the full estimator matches the independent integral: both connections
   // are combined by the balance heuristic, so no light is lost or counted
   // twice; and it is clearly above the emitter-sampling-only expectation,
   // i.e. continuation hits on the emitter do contribute
   {
      std::unique_ptr<Scene> pScene( newScene(
         "(0 0 0) (0 0 0)\n" + emitterLine + receiverLine ) );
      PathGuide guide( *pScene );
      RayTracer tracer( *pScene, &guide );
      Random random( 2024 );
      guide.beginPass();
      const int N = 200000;
      Vector3f sum;
      Vector3f sum2;
      for( int i = 0;  i < N;  ++i )
      {
         const Vector3f r( tracer.getRadiance(
            p + Vector3f( 0.0, 0.25, 0.0 ), Vector3f( 0.0, -1.0, 0.0 ),
            random ) );
         sum  = sum  + r;
         sum2 = sum2 + (r * r);
      }
      guide.finishPass();
      const Vector3f mean( sum / static_cast<real64>(N) );
      for( int c = 0;  c < 3;  ++c )
      {
         const real64 expected = (rho / PI) * emit[c] * intTotal;
         const real64 neeOnly  = (rho / PI) * emit[c] * intNee;
         const real64 var = (sum2[c] / N) - (mean[c] * mean[c]);
         const real64 tol = 5.0 * std::sqrt( var / N ) + 1e-12;
         CHECK( std::fabs( mean[c] - expected ) < tol );
         CHECK( mean[c] > neeOnly + (0.5 * (expected - neeOnly)) );
      }
   }

   // occlusion: a dark blocker between receiver and emitter blocks all
   // connections, so no light arrives
   {
      std::unique_ptr<Scene> pScene( newScene(
         "(0 0 0) (0 0 0)\n" + emitterLine + receiverLine +
         "(-3 0.5 -3) (-3 0.5 3) (3 0.5 -3) (0 0 0) (0 0 0)\n" ) );
      PathGuide guide( *pScene );
      RayTracer tracer( *pScene, &guide );
      Random random( 11 );
      guide.beginPass();
      Vector3f sum;
      for( int i = 0;  i < 2000;  ++i )
      {
         sum = sum + tracer.getRadiance( p + Vector3f( 0.0, 0.25, 0.0 ),
            Vector3f( 0.0, -1.0, 0.0 ), random );
      }
      guide.finishPass();
      CHECK( sum.isZero() );
   }

   // back face: the emitter facing away contributes nothing
   {
      std::unique_ptr<Scene> pScene( newScene(
         "(0 0 0) (0 0 0)\n" + emitterBackLine + receiverLine ) );
      PathGuide guide( *pScene );
      RayTracer tracer( *pScene, &guide );
      Random random( 13 );
      guide.beginPass();
      Vector3f sum;
      for( int i = 0;  i < 2000;  ++i )
      {
         sum = sum + tracer.getRadiance( p + Vector3f( 0.0, 0.25, 0.0 ),
            Vector3f( 0.0, -1.0, 0.0 ), random );
      }
      guide.finishPass();
      CHECK( sum.isZero() );
   }

   // primary ray straight onto the emitter keeps the full emission
   {
      std::unique_ptr<Scene> pScene( newScene(
         "(0 0 0) (0 0 0)\n" + emitterLine + receiverLine ) );
      PathGuide guide( *pScene );
      RayTracer tracer( *pScene, &guide );
      Random random( 17 );
      guide.beginPass();
      const Vector3f radiance( tracer.getRadiance(
         Vector3f( -0.5, 0.5, -0.5 ), Vector3f( 0.0, 1.0, 0.0 ), random ) );
      guide.finishPass();
      CHECK( nearVec( radiance, emit, 1e-9 ) );
   }
}




/// passes ---------------------------------------------------------------------

CASE(passes)
{
   std::unique_ptr<Scene> pScene( newScene(
      "(1 2 4) (0.1 0.1 0.1)\n"
      "(-10 0 -10) (-10 0 10) (10 0 -10) (.4 .5 .6) (0 0 0)\n" ) );
   const Triangle& floorTri = pScene->getTriangles()[0];
   const Vector3f eye( -1.0, 1.0, -1.0 );
   const Vector3f down( 0.0, -1.0, 0.0 );
   const Vector3f up( 0.0, 1.0, 0.0 );

   PathGuide guide( *pScene );
   RayTracer tracer( *pScene, &guide );

   // probe direction and its bin on the floor's upper side
   const Vector3f probe( Vector3f( 0.3, 0.8, 0.2 ).unitize() );
   const int probeBin = guide.getBinIndex( floorTri, up, probe );
   CHECK( probeBin >= 0 );

   // one pass with real recursive paths: the distribution is frozen inside
   // the pass and the training is published only at the end
   CHECK( guide.getTrainingCount() == 0 );
   guide.beginPass();
   const real64 pdfFrozen = guide.getPdf( floorTri, up, probe );
   {
      Random random( 99 );
      for( int i = 0;  i < 4000;  ++i )
      {
         tracer.getRadiance( eye, down, random );
      }
   }
   CHECK( guide.getTrainingCount() > 0 );
   CHECK( guide.getBinMass( floorTri, up, probeBin ) == 0.0 );
   CHECK( guide.getPdf( floorTri, up, probe ) == pdfFrozen );
   guide.finishPass();
   CHECK( guide.getBinMass( floorTri, up, probeBin ) > 0.0 );
   CHECK( guide.getPdf( floorTri, up, probe ) != pdfFrozen );
   const real64 pdfTrained = guide.getPdf( floorTri, up, probe );

   // a second guide on the same scene keeps independent state
   {
      PathGuide guide2( *pScene );
      CHECK( guide2.getBinMass( floorTri, up, probeBin ) == 0.0 );
      CHECK( guide2.getTrainingCount() == 0 );
      guide2.beginPass();
      {
         RayTracer tracer2( *pScene, &guide2 );
         Random random( 7 );
         for( int i = 0;  i < 2000;  ++i )
         {
            tracer2.getRadiance( eye, down, random );
         }
      }
      guide2.finishPass();
      CHECK( guide2.getBinMass( floorTri, up, probeBin ) > 0.0 );
      CHECK( guide.getBinMass( floorTri, up, probeBin ) > 0.0 );
      CHECK( guide.getPdf( floorTri, up, probe ) == pdfTrained );
   }

   // guides on different scenes are independent too
   {
      std::unique_ptr<Scene> pOther( newScene(
         "(1 1 1) (0 0 0)\n"
         "(0 0 0) (1 0 0) (0 1 0) (0.5 0.5 0.5) (0 0 0)\n" ) );
      PathGuide guideOther( *pOther );
      CHECK( guideOther.getTrainingCount() == 0 );
      CHECK( guideOther.getBinMass( pOther->getTriangles()[0],
         Vector3f( 0.0, 0.0, 1.0 ), 24 ) == 0.0 );
      CHECK( guide.getBinMass( floorTri, up, probeBin ) > 0.0 );
   }

   // clear returns to the zero-mass uniform state
   guide.clear();
   CHECK( guide.getTrainingCount() == 0 );
   CHECK( guide.getBinMass( floorTri, up, probeBin ) == 0.0 );
   CHECK( guide.getPdf( floorTri, up, probe ) == pdfFrozen );

   // multiple camera frames: each frame is one pass, publishing its
   // training at the end
   {
      std::istringstream cameraInput( std::string(
         "(0 1 0) (0 -1 0) 45\n" ) );
      const Camera camera( cameraInput );
      std::istringstream imageInput( std::string( "4 3\n" ) );
      Image image( imageInput );
      PathGuide frameGuide( *pScene );
      Random random( 7 );
      int32u lastCount = 0;
      for( int f = 0;  f < 3;  ++f )
      {
         camera.getFrame( *pScene, random, image, &frameGuide );
         CHECK( frameGuide.getTrainingCount() > lastCount );
         lastCount = frameGuide.getTrainingCount();
      }
      real64 massSum = 0.0;
      for( int b = 0;  b < 32;  ++b )
      {
         massSum += frameGuide.getBinMass( floorTri, up, b );
      }
      CHECK( massSum > 0.0 );
      // the accumulated image is non-black
      std::ostringstream ppm;
      image.getFormatted( ppm, 3 );
      const std::string raster( ppm.str() );
      const std::size_t data = raster.find( "255\n" );
      CHECK( raster.rfind( "P6\n", 0 ) == 0 );
      CHECK( data != std::string::npos );
      bool nonzero = false;
      for( std::size_t i = data + 4;  i < raster.size();  ++i )
      {
         nonzero |= raster[i] != 0;
      }
      CHECK( nonzero );
   }

   // variance reduction from training, estimated with real recursive paths:
   // a vertical diffuse sheet under a bright sky with a very dark ground
   {
      std::unique_ptr<Scene> pSheet( newScene(
         "(8 8 8) (0.00025 0.00025 0.00025)\n"
         "(0 -3 -3) (0 3 -3) (0 -3 3) (0.8 0.8 0.8) (0 0 0)\n" ) );
      const Triangle& sheet = pSheet->getTriangles()[0];
      const Vector3f sheetIn( 1.0, 0.0, 0.0 );
      PathGuide sheetGuide( *pSheet );
      RayTracer sheetTracer( *pSheet, &sheetGuide );
      const Vector3f origin( 2.0, -1.0, 0.0 );
      const Vector3f dir( -1.0, 0.0, 0.0 );

      const int SEEDS = 24;
      const int BATCH = 64;

      // cold-start batches: the empty guide stays frozen for the round
      std::vector<real64> cold;
      sheetGuide.beginPass();
      for( int s = 0;  s < SEEDS;  ++s )
      {
         Random random( 1000 + s );
         real64 sum = 0.0;
         for( int i = 0;  i < BATCH;  ++i )
         {
            sum += sheetTracer.getRadiance( origin, dir, random )[0];
         }
         cold.push_back( sum / BATCH );
      }
      sheetGuide.finishPass();
      sheetGuide.clear();

      // train over several passes with real recursive paths
      {
         Random random( 424242 );
         for( int t = 0;  t < 8;  ++t )
         {
            sheetGuide.beginPass();
            for( int i = 0;  i < 3000;  ++i )
            {
               sheetTracer.getRadiance( origin, dir, random );
            }
            sheetGuide.finishPass();
         }
      }
      CHECK( sheetGuide.getTrainingCount() > 0 );

      // direction frequencies of the trained guide at the sheet point
      int upCount   = 0;
      int downCount = 0;
      std::vector<int> binCounts( 32, 0 );
      {
         Random random( 5 );
         for( int i = 0;  i < 16000;  ++i )
         {
            Vector3f d;
            sheetGuide.sampleDirection( sheet, sheetIn, random, d );
            ++binCounts[ sheetGuide.getBinIndex( sheet, sheetIn, d ) ];
            (d[1] > 0.0 ? upCount : downCount) += 1;
         }
      }

      // trained batches: same fixed seeds, equal samples per batch
      std::vector<real64> trained;
      sheetGuide.beginPass();
      for( int s = 0;  s < SEEDS;  ++s )
      {
         Random random( 1000 + s );
         real64 sum = 0.0;
         for( int i = 0;  i < BATCH;  ++i )
         {
            sum += sheetTracer.getRadiance( origin, dir, random )[0];
         }
         trained.push_back( sum / BATCH );
      }
      sheetGuide.finishPass();

      real64 meanCold = 0.0;
      real64 meanTrained = 0.0;
      for( int s = 0;  s < SEEDS;  ++s )
      {
         meanCold    += cold[s];
         meanTrained += trained[s];
      }
      meanCold    /= SEEDS;
      meanTrained /= SEEDS;
      real64 varCold = 0.0;
      real64 varTrained = 0.0;
      for( int s = 0;  s < SEEDS;  ++s )
      {
         const real64 dc = cold[s]    - meanCold;
         const real64 dt = trained[s] - meanTrained;
         varCold    += dc * dc;
         varTrained += dt * dt;
      }
      varCold    /= (SEEDS - 1);
      varTrained /= (SEEDS - 1);

      std::printf( "GUIDE_DIRECTION_FREQ up=%d down=%d bins=", upCount,
         downCount );
      for( int b = 0;  b < 32;  ++b )
      {
         std::printf( "%s%d", b ? "," : "", binCounts[b] );
      }
      std::printf( "\n" );
      std::printf( "GUIDE_BATCH_MEANS cold=" );
      for( int s = 0;  s < SEEDS;  ++s )
      {
         std::printf( "%s%.4f", s ? "," : "", cold[s] );
      }
      std::printf( "\n" );
      std::printf( "GUIDE_BATCH_MEANS trained=" );
      for( int s = 0;  s < SEEDS;  ++s )
      {
         std::printf( "%s%.4f", s ? "," : "", trained[s] );
      }
      std::printf( "\n" );
      std::printf( "GUIDE_VARIANCE seeds=%d batch=%d coldMean=%.6f "
         "trainedMean=%.6f coldVar=%.6f trainedVar=%.6f\n",
         SEEDS, BATCH, meanCold, meanTrained, varCold, varTrained );

      // both estimate the same quantity, and training reduces the variance
      CHECK( std::fabs( meanCold - meanTrained ) < 0.25 * meanCold );
      CHECK( varTrained < varCold );
   }
}
