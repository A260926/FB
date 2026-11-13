/*------------------------------------------------------------------------------

   MiniLight C++ : minimal global illumination renderer
   Harrison Ainsworth / HXA7241 : 2006-2011, 2013

   http://www.hxa.name/minilight

------------------------------------------------------------------------------*/


#ifndef SurfacePoint_h
#define SurfacePoint_h


#include "Vector3f.hpp"




#include "hxa7241_minilight.hpp"
namespace hxa7241_minilight
{


/**
 * Surface point at a ray-object intersection.<br/><br/>
 *
 * All direction parameters are away from surface.<br/><br/>
 *
 * Constant.<br/><br/>
  *
 * @invariants
 * * pTriangle_m points to a Triangle (is not 0)
*/
class SurfacePoint
{
/// nested types ---------------------------------------------------------------
public:
   /**
    * Result of a guided continuation sample.<br/><br/>
    *
    * direction is the sampled continuation direction; throughput is the
    * path throughput weight reflectivity * cosTheta / (pi * survival * pdf)
    * (valid for the mixture density, not just cosine sampling); pdf is the
    * full mixture density q(direction) per unit solid angle; survival is
    * the russian roulette survival probability; cosTheta is direction dot
    * oriented normal.
    */
   struct GuidedBounce
   {
      Vector3f direction;
      Vector3f throughput;
      real64   pdf;
      real64   survival;
      real64   cosTheta;

      GuidedBounce() : direction(), throughput(), pdf( 0.0 ),
         survival( 0.0 ), cosTheta( 0.0 ) {}
   };


/// standard object services ---------------------------------------------------
public:
            SurfacePoint( const Triangle& triangle,
                          const Vector3f& position );

// use defaults
//         ~SurfacePoint();
//          SurfacePoint( const SurfacePoint& );
// SurfacePoint& operator=( const SurfacePoint& );


/// commands -------------------------------------------------------------------


/// queries --------------------------------------------------------------------
           Vector3f getEmission( const Vector3f& toPosition,
                                 const Vector3f& outDirection,
                                 const bool      isSolidAngle )           const;
           Vector3f getReflection( const Vector3f& inDirection,
                                   const Vector3f& inRadiance,
                                   const Vector3f& outDirection )         const;
           bool     getNextDirection( const Random&    random,
                                      const Vector3f&  inDirection,
                                      Vector3f&        outDirection_o,
                                      Vector3f&        color_o )          const;
           /**
            * Guided variant: samples the PathGuide mixture distribution and
            * reports the quantities needed for throughput and training.
            */
           bool     getNextDirection( const Random&    random,
                                      const Vector3f&  inDirection,
                                      const PathGuide& guide,
                                      GuidedBounce&    bounce_o )         const;

           const void* getHitId()                                         const;
           Vector3f    getPosition()                                      const;


/// fields ---------------------------------------------------------------------
private:
   const Triangle* pTriangle_m;
   Vector3f        position_m;
};


}//namespace




#endif//SurfacePoint_h
