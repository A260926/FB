/*------------------------------------------------------------------------------

   MiniLight C++ : minimal global illumination renderer
   Harrison Ainsworth / HXA7241 : 2006-2011, 2013

   http://www.hxa.name/minilight

------------------------------------------------------------------------------*/


#ifndef PathGuide_h
#define PathGuide_h


#include <unordered_map>
#include <vector>




#include "hxa7241_minilight.hpp"
namespace hxa7241_minilight
{


/**
 * Learned incident-light guide for path continuation directions.<br/><br/>
 *
 * For every triangle of one Scene, and for each of its two sides, a
 * hemisphere of incident directions is recorded as 32 equal-solid-angle bins
 * (8 azimuth x 4 cosine bands). The local frame is built from the triangle
 * tangent and the normal flipped towards the side the incoming ray arrives
 * from. Training accumulates a non-negative mass per bin; sampling uses a
 * direction distribution proportional to the learned masses (with a small
 * uniform floor), mixed 50/50 with the plain cosine distribution. All
 * densities are per unit solid angle.<br/><br/>
 *
 * Usage / call order:
 * - the object is owned by the caller and bound to one Scene at
 *   construction; it is not shared between scenes, and two guides on the
 *   same scene keep independent state;
 * - beginPass() opens a round (one Camera::getFrame is one round): during
 *   the round sampleDirection()/getPdf() read the distribution frozen at
 *   round start, and train() writes into a separate accumulation;
 * - finishPass() closes the round and publishes the accumulated training
 *   into the distribution used by the next round -- pixels later in the
 *   same round never see the new masses;
 * - clear() resets everything to the zero-mass (uniform) state;
 * - getBinMass()/getTrainingCount() are read-only views of the published
 *   distribution and the number of training contributions actually
 *   recorded.<br/><br/>
 *
 * Single-threaded only.
 *
 * @invariants
 * * live_m and accum_m have size triangles * 2 * BIN_COUNT, all >= 0
 * * during a pass, live_m is constant
 */
class PathGuide
{
/// standard object services ---------------------------------------------------
public:
            /**
             * Bind to a Scene. The Scene must outlive this object.
             */
   explicit PathGuide( const Scene& );

// use defaults
//         ~PathGuide();
private:
            PathGuide( const PathGuide& );
   PathGuide& operator=( const PathGuide& );
public:


/// commands -------------------------------------------------------------------
           /**
            * Open a round: freeze the readable distribution for the round.
            */
           void     beginPass();
           /**
            * Close a round: publish accumulated training masses so the next
            * round reads them.
            */
           void     finishPass();
           /**
            * Reset to zero masses (uniform distribution) and zero count.
            */
           void     clear();

           /**
            * Record a training mass for the bin of outDirection, on the side
            * of the triangle that inDirection arrives from. Negative or
            * non-finite masses are clamped to zero. Only valid while a pass
            * is open; takes effect (for sampling) at the next finishPass().
            */
           void     train( const Triangle& triangle,
                           const Vector3f& inDirection,
                           const Vector3f& outDirection,
                           real64          mass );


/// queries --------------------------------------------------------------------
           /**
            * Sample a continuation direction from the mixture
            * q = 0.5 * cosine + 0.5 * guide, around the side of the triangle
            * that inDirection arrives from. Returns the full mixture density
            * q(direction) per unit solid angle (always > 0), whichever
            * component produced the sample.
            */
           real64   sampleDirection( const Triangle& triangle,
                                     const Vector3f& inDirection,
                                     const Random&   random,
                                     Vector3f&       outDirection_o )   const;

           /**
            * Full mixture density q(direction) per unit solid angle, for the
            * side of the triangle that inDirection arrives from. Zero below
            * the local horizon.
            */
           real64   getPdf( const Triangle& triangle,
                            const Vector3f& inDirection,
                            const Vector3f& outDirection )               const;

           /**
            * Bin index [0, BIN_COUNT) of direction in the local frame of the
            * triangle side that inDirection arrives from; -1 if below the
            * local horizon.
            */
           int32    getBinIndex( const Triangle& triangle,
                                 const Vector3f& inDirection,
                                 const Vector3f& direction )             const;

           /**
            * Published (frozen) mass of one bin of the side of the triangle
            * that inDirection arrives from.
            */
           real64   getBinMass( const Triangle& triangle,
                                const Vector3f& inDirection,
                                int32           bin )                    const;

           /**
            * Number of training contributions actually recorded since
            * construction or the last clear().
            */
           int32u   getTrainingCount()                                    const;


/// constants ------------------------------------------------------------------
   static  int32    BIN_COUNT();     ///< 32
   static  int32    AZIMUTH_COUNT(); ///< 8
   static  int32    Z_COUNT();       ///< 4


/// implementation -------------------------------------------------------------
protected:
           int32u   storageIndex( const Triangle& triangle,
                                  const Vector3f& inDirection )          const;
           real64   getBinProbability( int32u sideBase,
                                       int32  bin )                      const;


/// fields ---------------------------------------------------------------------
private:
   const Scene*                          pScene_m;
   std::unordered_map<const void*,int32u> triangleIndex_m;

   // per (triangle, side, bin): published masses read during a pass, and
   // masses being accumulated by the current pass
   std::vector<real64>                   live_m;
   std::vector<real64>                   accum_m;

   int32u                                trainingCount_m;
   bool                                  isPassOpen_m;
};


}//namespace




#endif//PathGuide_h
