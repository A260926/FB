/*------------------------------------------------------------------------------

   MiniLight C++ : minimal global illumination renderer
   Harrison Ainsworth / HXA7241 : 2006-2011, 2013

   http://www.hxa.name/minilight

------------------------------------------------------------------------------*/


#include <math.h>
#include <stdexcept>
#include <algorithm>
#include <limits>

#include "Random.hpp"
#include "Camera.hpp"
#include "Scene.hpp"
#include "Image.hpp"
#include "Triangle.hpp"
#include "SurfacePoint.hpp"

#include "PathReuse.hpp"


using namespace hxa7241_minilight;
using namespace hxa7241_general;
using namespace hxa7241_graphics;


/// constants ------------------------------------------------------------------

namespace
{
   const real64 PI           = 3.14159265358979;
   const real64 GRAZE_EPS    = 1.0e-9;
   const real64 LENGTH_EPS   = 1.0e-9;
   const real64 POSITION_EPS = 1.0e-7;
}


/// internal records -----------------------------------------------------------

struct PathReuse::FirstHit
{
   FirstHit() : valid( false ), triangle( 0 ) {}

   bool             valid;
   Vector3f         position;
   Vector3f         cameraNormal;   // unit geometric normal facing the camera
   const Triangle*  triangle;
};


struct PathReuse::Candidate
{
   Candidate() : valid( false ), sideSign( 0 ), triangle( 0 ) {}

   bool             valid;
   Vector3f         position;
   int32            sideSign;       // +1: geometric normal faces the source
   const Triangle*  triangle;
   Vector3f         suffixB;        // outgoing estimate B at y
};


/// standard object services ---------------------------------------------------

PathReuse::PathReuse()
 : pCamera_m( 0 )
 , pScene_m( 0 )
 , width_m( 0 )
 , height_m( 0 )
 , maxDepth_m( 0 )
 , seed_m( 0u )
 , passes_m( 0 )
 , lastSuffixCount_m( 0 )
 , totalSuffixCount_m( 0 )
 , lastValidSourceCount_m( 0 )
 , densityErrorCount_m( 0 )
 , occludedConnectionCount_m( 0 )
 , sumDensity_m( 0.0 )
 , minPositiveDensity_m( 0.0 )
 , maxDensity_m( 0.0 )
{
}


// vectors of the nested (now-complete) records are destroyed here
PathReuse::~PathReuse()
{
}


PathReuse::PathReuse
(
   const Camera& camera,
   const Scene&  scene,
   const int32   width,
   const int32   height,
   const int32   maxDepth,
   const int32u  seed
)
 : pCamera_m( 0 )
 , pScene_m( 0 )
 , width_m( 0 )
 , height_m( 0 )
 , maxDepth_m( 0 )
 , seed_m( 0u )
 , passes_m( 0 )
 , lastSuffixCount_m( 0 )
 , totalSuffixCount_m( 0 )
 , lastValidSourceCount_m( 0 )
 , densityErrorCount_m( 0 )
 , occludedConnectionCount_m( 0 )
 , sumDensity_m( 0.0 )
 , minPositiveDensity_m( 0.0 )
 , maxDensity_m( 0.0 )
{
   bind( camera, scene, width, height, maxDepth, seed );
}


/// commands -------------------------------------------------------------------

void PathReuse::bind
(
   const Camera& camera,
   const Scene&  scene,
   const int32   width,
   const int32   height,
   const int32   maxDepth,
   const int32u  seed
)
{
   if( (width < 1) | (height < 1) )
      throw std::invalid_argument( "PathReuse: image dimensions must be >= 1" );
   if( (maxDepth < 2) | (maxDepth > 12) )
      throw std::invalid_argument( "PathReuse: max depth D must be in [2,12]" );
   if( !scene.isBackgroundBlack() )
      throw std::invalid_argument(
         "PathReuse: sky and ground background must both be black" );

   pCamera_m  = &camera;
   pScene_m   = &scene;
   width_m    = width;
   height_m   = height;
   maxDepth_m = maxDepth;
   seed_m     = seed;

   const int32u pixelCount = static_cast<int32u>(width_m * height_m);
   firstHits_m.resize( pixelCount );
   candidates_m.resize( pixelCount );
   linearImage_m.assign( pixelCount, Vector3f::ZERO() );
   passColors_m.assign( pixelCount, Vector3f::ZERO() );

   clear();
}


void PathReuse::clear()
{
   passes_m                   = 0;
   lastSuffixCount_m          = 0;
   totalSuffixCount_m         = 0;
   lastValidSourceCount_m     = 0;
   densityErrorCount_m        = 0;
   occludedConnectionCount_m  = 0;
   sumDensity_m               = 0.0;
   minPositiveDensity_m       = 0.0;
   maxDensity_m               = 0.0;

   for( int32u i = 0; i < firstHits_m.size(); ++i ) firstHits_m[i] = FirstHit();
   for( int32u i = 0; i < candidates_m.size(); ++i ) candidates_m[i] = Candidate();
   for( int32u i = 0; i < linearImage_m.size(); ++i )
      linearImage_m[i] = Vector3f::ZERO();
   for( int32u i = 0; i < passColors_m.size(); ++i )
      passColors_m[i] = Vector3f::ZERO();
}


namespace
{
   /**
    * Finite-depth cosine diffuse walk: no Russian roulette, no emitter
    * sampling. Every hit keeps that side's self-emission, then continues
    * multiplied by the albedo. A missed ray ends the walk, and the bound
    * background is guaranteed black. verticesLeft counts the current hit.
    */
   Vector3f diffuseSuffix
   (
      const Scene&    scene,
      Random&         random,
      const Vector3f& position,
      const Triangle& triangle,
      const Vector3f& inwardDirection,
      const int32     verticesLeft
   )
   {
      // self-emission of this hit, on the side reached by the walk
      Vector3f radiance;
      if( triangle.getNormal().dot( inwardDirection ) > 0.0 )
         radiance = triangle.getEmitivity();

      if( verticesLeft <= 1 ) return radiance;

      // cosine-weighted hemisphere sample around the arrival-side normal
      Vector3f nextDirection;
      {
         const real64 _2pr1 = PI * 2.0 * random.getReal64();
         const real64 sr2   = ::sqrt( random.getReal64() );
         const real64 cx = ::cos( _2pr1 ) * sr2;
         const real64 cy = ::sin( _2pr1 ) * sr2;
         const real64 cz = ::sqrt( 1.0 - (sr2 * sr2) );

         Vector3f normal ( triangle.getNormal()  );
         Vector3f tangent( triangle.getTangent() );
         normal = normal.dot( inwardDirection ) >= 0.0 ? normal : -normal;

         nextDirection = (tangent * cx) + (normal.cross( tangent ) * cy) +
            (normal * cz);
      }

      const Triangle* pHit = 0;
      Vector3f        hitPosition;
      scene.getIntersection( position, nextDirection,
         static_cast<const void*>( &triangle ), pHit, hitPosition );

      if( 0 == pHit ) return radiance;   // black background ends the walk

      const Vector3f continued( diffuseSuffix( scene, random, hitPosition,
         *pHit, -nextDirection, verticesLeft - 1 ) );

      return radiance + (triangle.getReflectivity() * continued);
   }
}


void PathReuse::freezeFirstHits
(
   const Camera& camera,
   const Scene&  scene,
   Random&       random
)
{
   const Vector3f eye( camera.getEyePoint() );

   // same pixel order and jitter as Camera::getFrame
   for( real64 fy = height_m;  fy-- > 0.0; )
   {
      for( real64 fx = width_m;  fx-- > 0.0; )
      {
         const int32  x = static_cast<int32>(fx);
         const int32  y = static_cast<int32>(fy);
         const int32u index = static_cast<int32u>(y * width_m + x);

         FirstHit& record = firstHits_m[index];
         record = FirstHit();

         const Vector3f rayDirection( camera.getPixelRayDirection( x, y,
            width_m, height_m, random ) );

         const Triangle* pHit = 0;
         Vector3f        hitPosition;
         scene.getIntersection( eye, rayDirection, 0, pHit, hitPosition );

         if( 0 != pHit )
         {
            Vector3f normal( pHit->getNormal() );
            if( normal.dot( eye - hitPosition ) < 0.0 ) normal = -normal;

            record.valid        = true;
            record.position     = hitPosition;
            record.cameraNormal = normal;
            record.triangle     = pHit;
         }
      }
   }
}


void PathReuse::generateCandidates
(
   const Scene& scene,
   Random&      random
)
{
   lastSuffixCount_m      = 0;
   lastValidSourceCount_m = 0;

   for( int32u i = 0; i < candidates_m.size(); ++i )
   {
      Candidate& candidate = candidates_m[i];
      candidate = Candidate();

      const FirstHit& source = firstHits_m[i];
      if( !source.valid ) continue;
      ++lastValidSourceCount_m;

      // one cosine-weighted edge from the camera-facing side of x
      Vector3f edgeDirection;
      {
         const real64 _2pr1 = PI * 2.0 * random.getReal64();
         const real64 sr2   = ::sqrt( random.getReal64() );
         const real64 cx = ::cos( _2pr1 ) * sr2;
         const real64 cy = ::sin( _2pr1 ) * sr2;
         const real64 cz = ::sqrt( 1.0 - (sr2 * sr2) );

         const Vector3f tangent( source.triangle->getTangent() );
         const Vector3f normal ( source.cameraNormal );
         edgeDirection = (tangent * cx) + (normal.cross( tangent ) * cy) +
            (normal * cz);
      }

      const Triangle* pHit = 0;
      Vector3f        hitPosition;
      scene.getIntersection( source.position, edgeDirection,
         static_cast<const void*>( source.triangle ), pHit, hitPosition );

      // a miss is a genuine empty (zero) candidate: it still keeps its slot
      if( 0 == pHit ) continue;

      const Vector3f geometricNormal( pHit->getNormal() );
      const real64   sideCos = geometricNormal.dot( -edgeDirection );
      const int32    sideSign = sideCos >= 0.0 ? 1 : -1;

      // exactly one real suffix per generated candidate; the suffix starts at
      // y with a D-1 vertex budget, so together with the frozen x the whole
      // path holds at most D surface vertices
      const Vector3f b( diffuseSuffix( scene, random, hitPosition, *pHit,
         -edgeDirection, maxDepth_m - 1 ) );
      ++lastSuffixCount_m;

      candidate.valid    = true;
      candidate.position = hitPosition;
      candidate.sideSign = sideSign;
      candidate.triangle = pHit;
      candidate.suffixB  = b;
   }

   totalSuffixCount_m += lastSuffixCount_m;
}


bool PathReuse::mergePass
(
   const Scene& scene,
   Image&       image
)
{
   densityErrorCount_m       = 0;
   occludedConnectionCount_m = 0;
   sumDensity_m              = 0.0;
   minPositiveDensity_m      = std::numeric_limits<real64>::infinity();
   maxDensity_m              = 0.0;

   const int32 pixelCount = static_cast<int32>(candidates_m.size());
   std::fill( passColors_m.begin(), passColors_m.end(), Vector3f::ZERO() );

   // transport check over the frozen candidate set: a genuinely generated,
   // non-degenerate candidate must have positive total density (its own
   // source ray reaches it)
   for( int32 j = 0; j < pixelCount; ++j )
   {
      if( !candidates_m[static_cast<int32u>(j)].valid ) continue;

      std::vector<int32> ownSources;
      getSourceSet( j, ownSources );
      real64 totalDensity = 0.0;
      for( int32u s = 0; s < ownSources.size(); ++s )
         totalDensity += sourceDensity( ownSources[s], scene, j );

      if( !(totalDensity > 0.0) )
      {
         ++densityErrorCount_m;
         return false;
      }

      sumDensity_m += totalDensity;
      if( totalDensity < minPositiveDensity_m ) minPositiveDensity_m = totalDensity;
      if( totalDensity > maxDensity_m ) maxDensity_m = totalDensity;
   }
   if( 0 == maxDensity_m ) minPositiveDensity_m = 0.0;

   // the whole candidate set is frozen, so post-processing pixels only read
   // candidates, never merged results
   for( int32 i = 0; i < pixelCount; ++i )
   {
      const int32 x = i % width_m;
      const int32 y = i / width_m;
      const FirstHit& receiver = firstHits_m[static_cast<int32u>(i)];

      Vector3f color;
      if( receiver.valid )
      {
         // first-hit emission on the camera-facing side
         const Vector3f eye( pCamera_m->getEyePoint() );
         const Vector3f outDirection( (eye - receiver.position).unitize() );
         color = SurfacePoint( *receiver.triangle, receiver.position )
            .getEmission( eye, outDirection, false );

         std::vector<int32> sources;
         getSourceSet( i, sources );
         for( int32u s = 0; s < sources.size(); ++s )
         {
            const int32 j = sources[s];
            if( !candidates_m[static_cast<int32u>(j)].valid )
               continue;   // empty/black candidate: zero, slot still counted

            // denominator over this receiver's own source set
            real64 totalDensity = 0.0;
            for( int32u t = 0; t < sources.size(); ++t )
               totalDensity += sourceDensity( sources[t], scene, j );

            const Vector3f contribution( receiverContribution( i, scene, j ) );
            if( contribution.isZero() )
            {
               // diagnostic: a finite connection blocked by another surface
               const Candidate& cand = candidates_m[static_cast<int32u>(j)];
               const Vector3f delta( cand.position - receiver.position );
               const real64 dist2 = delta.dot( delta );
               if( dist2 >= (LENGTH_EPS * LENGTH_EPS) )
               {
                  const Triangle* pHit = 0;
                  Vector3f hitPosition;
                  scene.getIntersection( receiver.position, delta.unitize(),
                     static_cast<const void*>( receiver.triangle ),
                     pHit, hitPosition );
                  if( (0 != pHit) & (pHit != cand.triangle) )
                     ++occludedConnectionCount_m;
               }
            }

            color = color + (contribution / totalDensity);
         }
      }

      passColors_m[static_cast<int32u>(i)] = color;
      linearImage_m[static_cast<int32u>(i)] =
         linearImage_m[static_cast<int32u>(i)] + color;
      image.addToPixel( x, y, color );   // one linear accumulation per pixel
   }

   return true;
}


bool PathReuse::runPass
(
   const Camera& camera,
   const Scene&  scene,
   Image&        image
)
{
   checkBinding( camera, scene, image.getWidth(), image.getHeight() );

   // a fresh per-pass deterministic stream; seed 0 reproduces the classic
   // Random constants
   Random random( seed_m + static_cast<int32u>( passes_m ) );

   freezeFirstHits( camera, scene, random );
   generateCandidates( scene, random );

   const bool ok = mergePass( scene, image );
   if( ok ) ++passes_m;

   return ok;
}


/// queries --------------------------------------------------------------------

int32 PathReuse::getCompletedPasses() const   { return passes_m; }
int32 PathReuse::getWidth() const             { return width_m; }
int32 PathReuse::getHeight() const            { return height_m; }
int32 PathReuse::getMaxDepth() const          { return maxDepth_m; }
bool  PathReuse::isBound() const              { return 0 != pScene_m; }

int32 PathReuse::getLastSuffixCount() const        { return lastSuffixCount_m; }
std::int64_t PathReuse::getTotalSuffixCount() const { return totalSuffixCount_m; }
int32 PathReuse::getLastValidSourceCount() const   { return lastValidSourceCount_m; }
int32 PathReuse::getDensityErrorCount() const      { return densityErrorCount_m; }


Vector3f PathReuse::getLinearPixel
(
   const int32 x,
   const int32 y
) const
{
   if( (x < 0) | (y < 0) | (x >= width_m) | (y >= height_m) )
      return Vector3f::ZERO();
   return linearImage_m[ static_cast<int32u>(y * width_m + x) ];
}


Vector3f PathReuse::getCandidatePixel
(
   const int32 x,
   const int32 y
) const
{
   if( (x < 0) | (y < 0) | (x >= width_m) | (y >= height_m) )
      return Vector3f::ZERO();
   return passColors_m[ static_cast<int32u>(y * width_m + x) ];
}


void PathReuse::getFirstHit
(
   const int32      x,
   const int32      y,
   Vector3f&        position_o,
   Vector3f&        cameraNormal_o,
   const Triangle*& triangle_o,
   bool&            valid_o
) const
{
   const FirstHit& record = firstHits_m[
      static_cast<int32u>(y * width_m + x)];
   position_o     = record.position;
   cameraNormal_o = record.cameraNormal;
   triangle_o     = record.triangle;
   valid_o        = record.valid;
}


void PathReuse::getCandidate
(
   const int32      x,
   const int32      y,
   Vector3f&        position_o,
   const Triangle*& triangle_o,
   int32&           sideSign_o,
   Vector3f&        suffixB_o,
   bool&            valid_o
) const
{
   const Candidate& cand = candidates_m[
      static_cast<int32u>(y * width_m + x)];
   position_o = cand.position;
   triangle_o = cand.triangle;
   sideSign_o = cand.sideSign;
   suffixB_o  = cand.suffixB;
   valid_o    = cand.valid;
}


void PathReuse::getSourceSet
(
   const int32         receiverPixel,
   std::vector<int32>& sources_o
) const
{
   sources_o.clear();

   const int32 x = receiverPixel % width_m;
   const int32 y = receiverPixel / width_m;

   const int32 offsets[5][2] = { {0,0}, {-1,0}, {1,0}, {0,-1}, {0,1} };
   for( int32 n = 0; n < 5; ++n )
   {
      const int32 sx = x + offsets[n][0];
      const int32 sy = y + offsets[n][1];
      if( (sx < 0) | (sy < 0) | (sx >= width_m) | (sy >= height_m) ) continue;

      const int32 index = sy * width_m + sx;
      // source slots are fixed by valid first hits, before candidate sampling
      if( !firstHits_m[static_cast<int32u>(index)].valid ) continue;
      if( std::find( sources_o.begin(), sources_o.end(), index ) ==
          sources_o.end() )
         sources_o.push_back( index );
   }
}


real64 PathReuse::sourceDensity
(
   const int32  sourcePixel,
   const Scene& scene,
   const int32  candidatePixel
) const
{
   const FirstHit& source = firstHits_m[
      static_cast<int32u>(sourcePixel)];
   const Candidate& cand = candidates_m[
      static_cast<int32u>(candidatePixel)];
   if( !source.valid || !cand.valid ) return 0.0;

   const Vector3f delta( cand.position - source.position );
   const real64   dist2 = delta.dot( delta );
   if( dist2 < (LENGTH_EPS * LENGTH_EPS) ) return 0.0;   // zero distance

   // degenerate: a source cannot support a candidate on its own surface
   if( source.triangle == cand.triangle ) return 0.0;

   const Vector3f w( delta / ::sqrt( dist2 ) );

   // source proposal cosine, against the normal facing its own camera
   const real64 sourceCos = source.cameraNormal.dot( w );
   if( sourceCos <= GRAZE_EPS ) return 0.0;               // grazing

   // the source's ray must first hit that exact triangle and position
   const Triangle* pHit = 0;
   Vector3f        hitPosition;
   scene.getIntersection( source.position, w,
      static_cast<const void*>( source.triangle ), pHit, hitPosition );
   if( 0 == pHit ) return 0.0;
   if( pHit != cand.triangle ) return 0.0;
   if( (hitPosition - cand.position).dot( hitPosition - cand.position ) >
       (POSITION_EPS * POSITION_EPS) ) return 0.0;

   // recorded side of y must match the side reached by this source
   const Vector3f ny( cand.triangle->getNormal() );
   const real64 yCos = ny.dot( -w );
   if( (yCos >= 0.0 ? 1 : -1) != cand.sideSign ) return 0.0;
   if( ::fabs( yCos ) <= GRAZE_EPS ) return 0.0;

   return (sourceCos / PI) * (::fabs( yCos ) / dist2);
}


Vector3f PathReuse::receiverContribution
(
   const int32  receiverPixel,
   const Scene& scene,
   const int32  candidatePixel
) const
{
   const FirstHit& receiver = firstHits_m[
      static_cast<int32u>(receiverPixel)];
   const Candidate& cand = candidates_m[
      static_cast<int32u>(candidatePixel)];
   if( !receiver.valid || !cand.valid ) return Vector3f::ZERO();

   // degenerate: connection onto the same surface is rejected
   if( receiver.triangle == cand.triangle ) return Vector3f::ZERO();

   const Vector3f delta( cand.position - receiver.position );
   const real64   dist2 = delta.dot( delta );
   if( dist2 < (LENGTH_EPS * LENGTH_EPS) ) return Vector3f::ZERO();

   const Vector3f w( delta / ::sqrt( dist2 ) );

   // receiver cosine at x: w points from x toward y on the camera-facing side
   const real64 receiverCos = receiver.cameraNormal.dot( w );
   if( receiverCos <= GRAZE_EPS ) return Vector3f::ZERO();

   const Vector3f ny( cand.triangle->getNormal() );
   const real64 yCosAbs = ::fabs( ny.dot( w ) );
   if( yCosAbs <= GRAZE_EPS ) return Vector3f::ZERO();

   // same-side condition: y must face x on the recorded side
   if( (ny.dot( -w ) >= 0.0 ? 1 : -1) != cand.sideSign )
      return Vector3f::ZERO();

   // finite connection visibility through the scene's own spatial index;
   // the endpoint triangle itself is not treated as an occluder
   const Triangle* pHit = 0;
   Vector3f        hitPosition;
   scene.getIntersection( receiver.position, w,
      static_cast<const void*>( receiver.triangle ), pHit, hitPosition );
   if( 0 != pHit )
   {
      if( pHit != cand.triangle ) return Vector3f::ZERO();
      if( (hitPosition - cand.position).dot( hitPosition - cand.position ) >
          (POSITION_EPS * POSITION_EPS) ) return Vector3f::ZERO();
   }

   const real64 geometry = (receiverCos * yCosAbs) / dist2;
   return (cand.suffixB * receiver.triangle->getReflectivity()) *
      (geometry / PI);
}


void PathReuse::getDiagnostics
(
   real64& sumDensity_o,
   real64& minPositiveDensity_o,
   real64& maxDensity_o,
   int32&  zeroDensityCandidates_o,
   int32&  occludedConnections_o
) const
{
   sumDensity_o             = sumDensity_m;
   minPositiveDensity_o     = minPositiveDensity_m;
   maxDensity_o             = maxDensity_m;
   zeroDensityCandidates_o  = densityErrorCount_m;
   occludedConnections_o    = occludedConnectionCount_m;
}


void PathReuse::checkBinding
(
   const Camera& camera,
   const Scene&  scene,
   const int32   width,
   const int32   height
) const
{
   if( !isBound() )
      throw std::invalid_argument( "PathReuse: not bound" );
   if( (&camera != pCamera_m) | (&scene != pScene_m) )
      throw std::invalid_argument(
         "PathReuse: camera/scene differ from the binding; bind() again" );
   if( (width != width_m) | (height != height_m) )
      throw std::invalid_argument(
         "PathReuse: image size differs from the binding; bind() again" );
}
