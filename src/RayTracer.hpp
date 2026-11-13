/*------------------------------------------------------------------------------

   MiniLight C++ : minimal global illumination renderer
   Harrison Ainsworth / HXA7241 : 2006-2011, 2013

   http://www.hxa.name/minilight

------------------------------------------------------------------------------*/


#ifndef RayTracer_h
#define RayTracer_h




#include "hxa7241_minilight.hpp"
namespace hxa7241_minilight
{


/**
 * Ray tracer for general light transport.<br/><br/>
 *
 * Traces a path with emitter sampling each step: A single chain of ray-steps
 * advances from the eye into the scene with one sampling of emitters at each
 * node.<br/><br/>
 *
 * When a PathGuide is supplied (not owned), continuation directions are
 * sampled from its learned mixture distribution, continuation-hit emission
 * is combined with emitter sampling by the balance heuristic, and traced
 * continuation rays train the guide. Without a guide, the original
 * cosine-sampling behaviour (and its random consumption) is kept exactly.
 *
 * Constant.
 *
 * @invariants
 * * pScene_m points to a Scene (is not 0)
 * * pGuide_m is 0 or points to a PathGuide owned by the caller
 */
class RayTracer
{
/// standard object services ---------------------------------------------------
public:
   explicit RayTracer( const Scene&,
                       PathGuide*   pGuide = 0 );

// use defaults
//         ~RayTracer();
//          RayTracer( const RayTracer& );
// RayTracer& operator=( const RayTracer& );


/// commands -------------------------------------------------------------------


/// queries --------------------------------------------------------------------
           Vector3f getRadiance( const Vector3f& rayOrigin,
                                 const Vector3f& rayDirection,
                                 const Random&   random,
                                 const void*     lastHit = 0 )            const;


/// implementation -------------------------------------------------------------
protected:
           Vector3f sampleEmitters( const Vector3f&     rayDirection,
                                    const SurfacePoint& surfacePoint,
                                    const Random&       random )          const;
           Vector3f sampleEmittersGuided( const Vector3f&     rayDirection,
                                          const SurfacePoint& surfacePoint,
                                          const Triangle&     triangle,
                                          const Random&       random )     const;
           Vector3f getGuidedRadiance( const Vector3f& rayOrigin,
                                       const Vector3f& rayDirection,
                                       const Random&   random,
                                       const void*     lastHit,
                                       real64          inPdf,
                                       Vector3f&       trainRadiance_o )  const;


/// fields ---------------------------------------------------------------------
private:
   const Scene* pScene_m;
   PathGuide*   pGuide_m;
};


}//namespace




#endif//RayTracer_h
