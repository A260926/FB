/*------------------------------------------------------------------------------

   MiniLight C++ : minimal global illumination renderer
   Harrison Ainsworth / HXA7241 : 2006-2011, 2013

   http://www.hxa.name/minilight

------------------------------------------------------------------------------*/


#include "Vector3f.hpp"
#include "Scene.hpp"
#include "SurfacePoint.hpp"
#include "PathGuide.hpp"
#include "Triangle.hpp"

#include "RayTracer.hpp"


using namespace hxa7241_minilight;




/// constants ------------------------------------------------------------------

namespace
{
   // ITU-R BT.709 standard RGB luminance weighting
   const Vector3f RGB_LUMINANCE( 0.2126, 0.7152, 0.0722 );
}




/// standard object services ---------------------------------------------------

RayTracer::RayTracer
(
   const Scene& scene,
   PathGuide*   pGuide
)
 : pScene_m( &scene )
 , pGuide_m( pGuide )
{
}




/// queries --------------------------------------------------------------------

Vector3f RayTracer::getRadiance
(
   const Vector3f& rayOrigin,
   const Vector3f& rayDirection,
   const Random&   random,
   const void*     lastHit
) const
{
   // guided mode
   if( 0 != pGuide_m )
   {
      Vector3f trainRadiance;
      return getGuidedRadiance( rayOrigin, rayDirection, random, lastHit,
         0.0, trainRadiance );
   }

   // original mode (unchanged)

   // intersect ray with scene
   const Triangle* pHitObject = 0;
   Vector3f        hitPosition;
   pScene_m->getIntersection( rayOrigin, rayDirection, lastHit,
      pHitObject, hitPosition );

   Vector3f radiance;
   if( 0 != pHitObject )
   {
      // make surface point of intersection
      SurfacePoint surfacePoint( *pHitObject, hitPosition );

      // local emission only for first-hit
      radiance = ( 0 != lastHit ? Vector3f::ZERO() :
         surfacePoint.getEmission( rayOrigin, -rayDirection, false ) );

      // add emitter sample
      radiance = radiance + sampleEmitters( rayDirection, surfacePoint, random);

      // add recursive reflection
      //
      // single hemisphere sample, ideal diffuse BRDF:
      //    reflected = (inradiance * pi) * (cos(in) / pi * color) *
      //       reflectance
      // -- reflectance magnitude is 'scaled' by the russian roulette,
      // cos is importance sampled (both done by SurfacePoint),
      // and the pi and 1/pi cancel out -- leaving just:
      //    inradiance * reflectance color
      Vector3f nextDirection;
      Vector3f color;
      // check surface reflects ray
      if( surfacePoint.getNextDirection( random, -rayDirection,
         nextDirection, color ) )
      {
         // recurse
         radiance = radiance + (color * getRadiance( surfacePoint.getPosition(),
            nextDirection, random, surfacePoint.getHitId() ));
      }
   }
   else
   {
      // no hit: default/background scene emission
      radiance = pScene_m->getDefaultEmission( -rayDirection );
   }

   return radiance;
}



/// implementation -------------------------------------------------------------

Vector3f RayTracer::sampleEmitters
(
   const Vector3f&     rayDirection,
   const SurfacePoint& surfacePoint,
   const Random&       random
) const
{
   Vector3f radiance;

   // single emitter sample, ideal diffuse BRDF:
   //    reflected = (emitivity * solidangle) * (emitterscount) *
   //       (cos(emitdirection) / pi * reflectivity)
   // -- SurfacePoint does the first and last parts (in separate methods)

   // get position on an emitter
   Vector3f        emitterPosition;
   const Triangle* emitterId = 0;
   pScene_m->getEmitter( random, emitterPosition, emitterId );

   // check an emitter was found
   if( 0 != emitterId )
   {
      // make direction to emit point
      const Vector3f emitDirection( (emitterPosition -
         surfacePoint.getPosition()).unitize() );

      // send shadow ray
      const Triangle* pHitObject = 0;
      Vector3f        hitPosition;
      pScene_m->getIntersection( surfacePoint.getPosition(), emitDirection,
         surfacePoint.getHitId(), pHitObject, hitPosition );

      // if unshadowed, get inward emission value
      Vector3f emissionIn;
      if( (0 == pHitObject) | (emitterId == pHitObject) )
      {
         emissionIn = SurfacePoint( *emitterId, emitterPosition ).getEmission(
            surfacePoint.getPosition(), -emitDirection, true );
      }

      // get amount reflected by surface
      radiance = surfacePoint.getReflection( emitDirection, emissionIn *
         static_cast<real64>(pScene_m->getEmittersCount()), -rayDirection );
   }

   return radiance;
}


Vector3f RayTracer::getGuidedRadiance
(
   const Vector3f& rayOrigin,
   const Vector3f& rayDirection,
   const Random&   random,
   const void*     lastHit,
   const real64    inPdf,
   Vector3f&       trainRadiance_o
) const
{
   // intersect ray with scene
   const Triangle* pHitObject = 0;
   Vector3f        hitPosition;
   pScene_m->getIntersection( rayOrigin, rayDirection, lastHit,
      pHitObject, hitPosition );

   Vector3f radiance;
   trainRadiance_o = Vector3f::ZERO();

   if( 0 != pHitObject )
   {
      // make surface point of intersection
      SurfacePoint surfacePoint( *pHitObject, hitPosition );

      // emission of the hit surface: full for a first/camera hit; for a
      // continuation hit, weighted against emitter sampling with the
      // balance heuristic (the two strategies compete per connection)
      const Vector3f emission( surfacePoint.getEmission( rayOrigin,
         -rayDirection, false ) );
      real64 emissionWeight = 1.0;
      if( 0 != lastHit )
      {
         // emitter-sample density of this connection, per unit solid angle
         // at the departure vertex: distance^2 / (emitters * area * cos)
         real64 lightPdf = 0.0;
         const int32  emitterCount = pScene_m->getEmittersCount();
         const real64 area         = pHitObject->getArea();
         const real64 cosLight     =
            (-rayDirection).dot( pHitObject->getNormal() );
         if( (emitterCount > 0) & (area > 0.0) & (cosLight > 0.0) )
         {
            const Vector3f ray( hitPosition - rayOrigin );
            lightPdf = ray.dot( ray ) / (static_cast<real64>(emitterCount) *
               area * cosLight);
         }
         const real64 denominator = inPdf + lightPdf;
         emissionWeight = denominator > 0.0 ? inPdf / denominator : 1.0;
      }
      radiance        = emission * emissionWeight;
      // the training radiance sees the emission unweighted
      trainRadiance_o = emission;

      // add emitter sample (balance-heuristic weighted)
      const Vector3f emitterSample( sampleEmittersGuided( rayDirection,
         surfacePoint, *pHitObject, random ) );
      radiance        = radiance + emitterSample;
      trainRadiance_o = trainRadiance_o + emitterSample;

      // add recursive reflection, guided
      //
      // single mixture sample, ideal diffuse BRDF:
      //    reflected = inradiance * (reflectance / pi) * cos(in)
      // throughput divides by the russian roulette survival probability and
      // by the actual mixture density q (done by SurfacePoint)
      SurfacePoint::GuidedBounce bounce;
      if( surfacePoint.getNextDirection( random, -rayDirection, *pGuide_m,
         bounce ) )
      {
         // recurse
         Vector3f childTrain;
         const Vector3f childRadiance( getGuidedRadiance(
            surfacePoint.getPosition(), bounce.direction, random,
            surfacePoint.getHitId(), bounce.pdf, childTrain ) );

         radiance        = radiance + (bounce.throughput * childRadiance);
         trainRadiance_o = trainRadiance_o + (bounce.throughput * childTrain);

         // train the guide from the actually traced continuation ray:
         // mass = Y(Li) * cos(theta) / (survival * q), where Li is the
         // incident radiance returned along the ray (without this vertex's
         // reflectivity, camera throughput, or the edge's emission weight)
         const real64 y = childTrain.dot( RGB_LUMINANCE );
         pGuide_m->train( *pHitObject, -rayDirection, bounce.direction,
            y * bounce.cosTheta / (bounce.survival * bounce.pdf) );
      }
   }
   else
   {
      // no hit: default/background scene emission -- no explicit sampling
      // strategy exists for it, so it keeps its full radiance
      radiance        = pScene_m->getDefaultEmission( -rayDirection );
      trainRadiance_o = radiance;
   }

   return radiance;
}


Vector3f RayTracer::sampleEmittersGuided
(
   const Vector3f&     rayDirection,
   const SurfacePoint& surfacePoint,
   const Triangle&     triangle,
   const Random&       random
) const
{
   Vector3f radiance;

   // single emitter sample, ideal diffuse BRDF, as in sampleEmitters, but
   // combined with the continuation distribution by the balance heuristic

   // get position on an emitter
   Vector3f        emitterPosition;
   const Triangle* emitterId = 0;
   pScene_m->getEmitter( random, emitterPosition, emitterId );

   // check an emitter was found
   if( 0 != emitterId )
   {
      // make direction to emit point
      const Vector3f emitDirection( (emitterPosition -
         surfacePoint.getPosition()).unitize() );

      // send shadow ray
      const Triangle* pHitObject = 0;
      Vector3f        hitPosition;
      pScene_m->getIntersection( surfacePoint.getPosition(), emitDirection,
         surfacePoint.getHitId(), pHitObject, hitPosition );

      // if unshadowed, get inward emission value
      Vector3f emissionIn;
      if( (0 == pHitObject) | (emitterId == pHitObject) )
      {
         emissionIn = SurfacePoint( *emitterId, emitterPosition ).getEmission(
            surfacePoint.getPosition(), -emitDirection, true );
      }

      // emitter-sample density of this connection, per unit solid angle at
      // this vertex: distance^2 / (emitters * area * emitter cosine)
      real64 lightPdf = 0.0;
      const real64 cosLight = (-emitDirection).dot( emitterId->getNormal() );
      const real64 area     = emitterId->getArea();
      if( (cosLight > 0.0) & (area > 0.0) )
      {
         const Vector3f ray( emitterPosition - surfacePoint.getPosition() );
         lightPdf = ray.dot( ray ) / (
            static_cast<real64>(pScene_m->getEmittersCount()) * area *
            cosLight );
      }

      // balance heuristic between emitter sampling and the continuation
      // mixture actually used at this vertex (russian roulette survival is
      // not part of these densities)
      const real64 q = pGuide_m->getPdf( triangle, -rayDirection,
         emitDirection );
      const real64 denominator = lightPdf + q;
      const real64 weight      = denominator > 0.0 ?
         lightPdf / denominator : 1.0;

      // get amount reflected by surface
      radiance = surfacePoint.getReflection( emitDirection, emissionIn *
         (static_cast<real64>(pScene_m->getEmittersCount()) * weight),
         -rayDirection );
   }

   return radiance;
}
