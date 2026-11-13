/*------------------------------------------------------------------------------

   MiniLight C++ : minimal global illumination renderer
   Harrison Ainsworth / HXA7241 : 2006-2011, 2013

   http://www.hxa.name/minilight

------------------------------------------------------------------------------*/


#include <math.h>

#include "Random.hpp"
#include "Scene.hpp"
#include "Triangle.hpp"

#include "PathGuide.hpp"


using namespace hxa7241_minilight;




/// constants ------------------------------------------------------------------

namespace
{
   const real64 PI     = 3.14159265358979;
   const real64 TWO_PI = PI * 2.0;

   // hemisphere split into 8 azimuth x 4 cosine bands of equal solid angle
   const int32 AZIMUTH_BIN_COUNT = 8;
   const int32 Z_BIN_COUNT       = 4;
   const int32 BIN_COUNT_TOTAL = AZIMUTH_BIN_COUNT * Z_BIN_COUNT;

   // solid angle of one bin: (2pi/8) * (1/4) = pi/16
   const real64 BIN_SOLID_ANGLE = (TWO_PI / real64(AZIMUTH_BIN_COUNT)) *
      (1.0 / real64(Z_BIN_COUNT));

   // uniform floor mixed into the learned bin probabilities
   const real64 MASS_FLOOR = 0.0001;

   const int32u INVALID_INDEX = int32u( 0xFFFFFFFFu );
}


/// helpers --------------------------------------------------------------------

namespace
{
   // local frame: triangle tangent, and normal flipped towards the side the
   // incoming ray arrives from
   void makeFrame( const Triangle& triangle,
                   const Vector3f& inDirection,
                   Vector3f&       tangent_o,
                   Vector3f&       bitangent_o,
                   Vector3f&       normal_o )
   {
      normal_o = triangle.getNormal();
      normal_o = normal_o.dot( inDirection ) >= 0.0 ? normal_o : -normal_o;
      tangent_o = triangle.getTangent();
      bitangent_o = normal_o.cross( tangent_o );
   }


   // bin of a direction in local coordinates; -1 if below the horizon
   int32 binFromLocal( const real64 x,
                       const real64 y,
                       const real64 z )
   {
      if( z < 0.0 )
      {
         return -1;
      }

      int32 binZ = static_cast<int32>( z * static_cast<real64>(Z_BIN_COUNT) );
      binZ = binZ < Z_BIN_COUNT ? binZ : Z_BIN_COUNT - 1;

      real64 phi = ::atan2( y, x );
      phi = phi < 0.0 ? phi + TWO_PI : phi;
      int32 binAz = static_cast<int32>( phi *
         (static_cast<real64>(AZIMUTH_BIN_COUNT) / TWO_PI) );
      binAz = binAz < AZIMUTH_BIN_COUNT ? binAz : AZIMUTH_BIN_COUNT - 1;

      return (binZ * AZIMUTH_BIN_COUNT) + binAz;
   }
}




/// standard object services ---------------------------------------------------

PathGuide::PathGuide
(
   const Scene& scene
)
 : pScene_m        ( &scene )
 , triangleIndex_m ()
 , live_m          ()
 , accum_m         ()
 , trainingCount_m ( 0 )
 , isPassOpen_m    ( false )
{
   const std::vector<Triangle>& triangles = scene.getTriangles();
   for( int32u i = 0;  i < triangles.size();  ++i )
   {
      triangleIndex_m[ static_cast<const void*>(&(triangles[i])) ] = i;
   }

   const int32u size = static_cast<int32u>( triangles.size() ) *
      2u * static_cast<int32u>( BIN_COUNT_TOTAL );
   live_m.resize( size, 0.0 );
   accum_m.resize( size, 0.0 );
}




/// commands -------------------------------------------------------------------

void PathGuide::beginPass()
{
   isPassOpen_m = true;
}


void PathGuide::finishPass()
{
   // publish the masses trained during this pass
   for( int32u i = 0;  i < live_m.size();  ++i )
   {
      live_m[i] += accum_m[i];
      accum_m[i] = 0.0;
   }

   isPassOpen_m = false;
}


void PathGuide::clear()
{
   for( int32u i = 0;  i < live_m.size();  ++i )
   {
      live_m[i]  = 0.0;
      accum_m[i] = 0.0;
   }

   trainingCount_m = 0;
   isPassOpen_m    = false;
}


void PathGuide::train
(
   const Triangle& triangle,
   const Vector3f& inDirection,
   const Vector3f& outDirection,
   real64          mass
)
{
   // only non-negative, finite masses
   mass = (mass >= 0.0) && (mass * 0.0 == 0.0) ? mass : 0.0;

   Vector3f tangent, bitangent, normal;
   makeFrame( triangle, inDirection, tangent, bitangent, normal );

   const int32 bin = binFromLocal( outDirection.dot( tangent ),
      outDirection.dot( bitangent ), outDirection.dot( normal ) );
   if( bin < 0 )
   {
      return;
   }

   const int32u base = storageIndex( triangle, inDirection );
   if( INVALID_INDEX == base )
   {
      return;
   }

   accum_m[ base + static_cast<int32u>(bin) ] += mass;
   ++trainingCount_m;
}




/// queries --------------------------------------------------------------------

real64 PathGuide::sampleDirection
(
   const Triangle& triangle,
   const Vector3f& inDirection,
   const Random&   random,
   Vector3f&       outDirection_o
) const
{
   Vector3f tangent, bitangent, normal;
   makeFrame( triangle, inDirection, tangent, bitangent, normal );

   const int32u base = storageIndex( triangle, inDirection );

   Vector3f local;
   // mixture component selection: half cosine, half guide
   if( random.getReal64() < 0.5 )
   {
      // cosine-weighted hemisphere sample
      const real64 _2pr1 = TWO_PI * random.getReal64();
      const real64 sr2   = ::sqrt( random.getReal64() );

      local = Vector3f( ::cos( _2pr1 ) * sr2, ::sin( _2pr1 ) * sr2,
         ::sqrt( 1.0 - (sr2 * sr2) ) );
   }
   else
   {
      // guide sample: choose a bin by its probability
      const real64 rBin = random.getReal64();
      real64 cumulative = 0.0;
      int32  bin        = BIN_COUNT_TOTAL - 1;
      for( int32 b = 0;  b < BIN_COUNT_TOTAL;  ++b )
      {
         cumulative += getBinProbability( base, b );
         if( rBin < cumulative )
         {
            bin = b;
            break;
         }
      }

      // uniform (in solid angle) inside the bin
      const real64 phi = (static_cast<real64>(bin % AZIMUTH_BIN_COUNT) +
         random.getReal64()) * (TWO_PI / static_cast<real64>(AZIMUTH_BIN_COUNT));
      const real64 z   = (static_cast<real64>(bin / AZIMUTH_BIN_COUNT) +
         random.getReal64()) / static_cast<real64>(Z_BIN_COUNT);
      const real64 rxy = ::sqrt( 1.0 - (z * z) );

      local = Vector3f( ::cos( phi ) * rxy, ::sin( phi ) * rxy, z );
   }

   outDirection_o = (tangent * local[0]) + (bitangent * local[1]) +
      (normal * local[2]);

   // full mixture density at the sampled direction, per unit solid angle
   const int32 binOut = binFromLocal( local[0], local[1], local[2] );
   const real64 pdfGuide = binOut >= 0 ?
      getBinProbability( base, binOut ) / BIN_SOLID_ANGLE : 0.0;
   return (0.5 * (local[2] / PI)) + (0.5 * pdfGuide);
}


real64 PathGuide::getPdf
(
   const Triangle& triangle,
   const Vector3f& inDirection,
   const Vector3f& outDirection
) const
{
   Vector3f tangent, bitangent, normal;
   makeFrame( triangle, inDirection, tangent, bitangent, normal );

   const real64 z = outDirection.dot( normal );
   if( z < 0.0 )
   {
      return 0.0;
   }

   const int32 bin = binFromLocal( outDirection.dot( tangent ),
      outDirection.dot( bitangent ), z );
   const real64 pdfGuide = bin >= 0 ? getBinProbability(
      storageIndex( triangle, inDirection ), bin ) / BIN_SOLID_ANGLE : 0.0;

   return (0.5 * (z / PI)) + (0.5 * pdfGuide);
}


int32 PathGuide::getBinIndex
(
   const Triangle& triangle,
   const Vector3f& inDirection,
   const Vector3f& direction
) const
{
   Vector3f tangent, bitangent, normal;
   makeFrame( triangle, inDirection, tangent, bitangent, normal );

   return binFromLocal( direction.dot( tangent ),
      direction.dot( bitangent ), direction.dot( normal ) );
}


real64 PathGuide::getBinMass
(
   const Triangle& triangle,
   const Vector3f& inDirection,
   const int32     bin
) const
{
   const int32u base = storageIndex( triangle, inDirection );
   if( (INVALID_INDEX == base) | (bin < 0) | (bin >= BIN_COUNT_TOTAL) )
   {
      return 0.0;
   }

   return live_m[ base + static_cast<int32u>(bin) ];
}


int32u PathGuide::getTrainingCount() const
{
   return trainingCount_m;
}




/// constants ------------------------------------------------------------------

int32 PathGuide::BIN_COUNT()
{
   return BIN_COUNT_TOTAL;
}


int32 PathGuide::AZIMUTH_COUNT()
{
   return AZIMUTH_BIN_COUNT;
}


int32 PathGuide::Z_COUNT()
{
   return Z_BIN_COUNT;
}




/// implementation -------------------------------------------------------------

int32u PathGuide::storageIndex
(
   const Triangle& triangle,
   const Vector3f& inDirection
) const
{
   const std::unordered_map<const void*,int32u>::const_iterator iter =
      triangleIndex_m.find( static_cast<const void*>( &triangle ) );
   if( iter == triangleIndex_m.end() )
   {
      return INVALID_INDEX;
   }

   // side 0: geometric normal side; side 1: opposite side
   const int32u side = triangle.getNormal().dot( inDirection ) >= 0.0 ?
      0u : 1u;

   return ((iter->second * 2u) + side) * static_cast<int32u>( BIN_COUNT_TOTAL );
}


real64 PathGuide::getBinProbability
(
   const int32u base,
   const int32  bin
) const
{
   if( INVALID_INDEX == base )
   {
      return 1.0 / static_cast<real64>( BIN_COUNT_TOTAL );
   }

   real64 sum = 0.0;
   for( int32 b = 0;  b < BIN_COUNT_TOTAL;  ++b )
   {
      sum += live_m[ base + static_cast<int32u>(b) ];
   }

   // with mass: learned probabilities with a small uniform floor;
   // without mass: uniform hemisphere
   return sum > 0.0 ?
      ((live_m[ base + static_cast<int32u>(bin) ] / sum) +
         (MASS_FLOOR / static_cast<real64>( BIN_COUNT_TOTAL ))) /
         (1.0 + MASS_FLOOR) :
      1.0 / static_cast<real64>( BIN_COUNT_TOTAL );
}
