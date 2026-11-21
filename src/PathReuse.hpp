/*------------------------------------------------------------------------------

   MiniLight C++ : minimal global illumination renderer
   Harrison Ainsworth / HXA7241 : 2006-2011, 2013

   http://www.hxa.name/minilight

------------------------------------------------------------------------------*/


#ifndef PathReuse_h
#define PathReuse_h


// This header is force-included into the C test driver (task_support/driver.c)
// as well as every C++ translation unit, so it must stay C-safe outside the
// __cplusplus block.
#ifdef __cplusplus


#include <cstdint>
#include <vector>

#include "Vector3f.hpp"
#include "hxa7241_minilight.hpp"


namespace hxa7241_general
{
   class Random;
}


namespace hxa7241_minilight
{


// forward declarations
class Camera;
class Scene;
class Image;
class Triangle;


/**
 * CPU diffuse suffix-reconnection path reuse, caller-owned per-pass state.<br/><br/>
 *
 * Each pass freezes the camera first-hit of every pixel, generates one
 * cosine-weighted candidate (and one real finite-depth diffuse suffix) per
 * valid source pixel, then reconnects every receiver pixel to the candidate
 * points of itself and its four cardinal neighbours using area-density MIS
 * weights. No Russian roulette, no explicit emitter sampling: only a bounded
 * cosine random walk whose emission is collected at every hit.<br/><br/>
 *
 * Instances are independent. A state object is bound to one Camera / Scene /
 * Image size at construction; binding a different size or Scene is rejected
 * rather than reusing stale cache.
 */
class PathReuse
{
/// standard object services ---------------------------------------------------
public:
   /** Construct unbound state; call bind() before rendering. */
   PathReuse();

   /** Construct and bind in one step (throws std::invalid_argument on error). */
   PathReuse( const Camera& camera,
              const Scene&  scene,
              int32         width,
              int32         height,
              int32         maxDepth,
              int32u        seed = 0u );

   /** Defined in the .cpp, where the nested records are complete. */
   ~PathReuse();

// use defaults
//          PathReuse( const PathReuse& );
// PathReuse& operator=( const PathReuse& );


/// commands -------------------------------------------------------------------
   /**
    * Bind (or re-bind) state. Clears every accumulated result. Throws
    * std::invalid_argument when the configuration is unsupported.
    */
   void  bind( const Camera& camera,
               const Scene&  scene,
               int32         width,
               int32         height,
               int32         maxDepth,
               int32u        seed = 0u );

   /** Reset all caches and counters without changing the binding. */
   void  clear();

   /**
    * Run one full reuse pass: regenerate the whole candidate set, then merge
    * one linear colour per pixel into the image and the linear accumulator.
    * Returns false (and leaves the image untouched) on a transport error such
    * as a genuine non-degenerate candidate whose total source density is zero.
    */
   bool  runPass( const Camera& camera,
                  const Scene&  scene,
                  Image&        image );


/// queries --------------------------------------------------------------------
   int32    getCompletedPasses()                                     const;
   int32    getWidth()                                                const;
   int32    getHeight()                                               const;
   int32    getMaxDepth()                                             const;
   bool     isBound()                                                 const;

   /** Number of real diffuse suffixes traced by the most recent pass. */
   int32    getLastSuffixCount()                                      const;
   /** Total number of real diffuse suffixes traced over every pass. */
   std::int64_t getTotalSuffixCount()                                  const;
   /** Number of valid (first-hit) source pixels seen by the last pass. */
   int32    getLastValidSourceCount()                                 const;
   /** Number of candidates rejected with zero total density (transport errors). */
   int32    getDensityErrorCount()                                    const;

   /** Linear accumulated colour of a pixel (sum over passes, no division). */
   Vector3f getLinearPixel( int32 x, int32 y )                        const;
   /** Candidate colour produced for a pixel by the most recent pass. */
   Vector3f getCandidatePixel( int32 x, int32 y )                     const;

   /** Frozen first-hit record of a pixel from the most recent pass. */
   void     getFirstHit( int32                 x,
                         int32                 y,
                         Vector3f&             position_o,
                         Vector3f&             cameraNormal_o,
                         const Triangle*&      triangle_o,
                         bool&                 valid_o )               const;

   /** Frozen candidate record of a pixel from the most recent pass. */
   void     getCandidate( int32                 x,
                          int32                 y,
                          Vector3f&             position_o,
                          const Triangle*&      triangle_o,
                          int32&                sideSign_o,
                          Vector3f&             suffixB_o,
                          bool&                 valid_o )              const;

   /**
    * Area density q_k(Y) that source pixel k contributes at candidate point Y
    * (the candidate recorded for sourcePixel), or 0 when the ray from k does
    * not first hit the recorded triangle/side at the recorded position.
    */
   real64   sourceDensity( int32         sourcePixel,
                           const Scene&  scene,
                           int32         candidatePixel )              const;

   /**
    * Unweighted receiver contribution F_i(Y_j) (the MIS weight is applied by
    * the caller), or 0 when the finite connection is degenerate, occluded, or
    * joins opposite sides of the candidate triangle.
    */
   Vector3f receiverContribution( int32         receiverPixel,
                                  const Scene&  scene,
                                  int32         candidatePixel )       const;

   /** Source pixel indices (deduplicated, ascending) for receiver pixel i. */
   void     getSourceSet( int32                 receiverPixel,
                          std::vector<int32>&   sources_o )            const;

   /** Density / connection diagnostics of the most recent pass. */
   void     getDiagnostics( real64& sumDensity_o,
                            real64& minPositiveDensity_o,
                            real64& maxDensity_o,
                            int32&  zeroDensityCandidates_o,
                            int32&  occludedConnections_o )           const;


/// implementation -------------------------------------------------------------
protected:
   struct FirstHit;
   struct Candidate;

   void  freezeFirstHits( const Camera& camera,
                          const Scene&  scene,
                          Random&       random );
   void  generateCandidates( const Scene& scene,
                             Random&      random );
   bool  mergePass( const Scene& scene, Image& image );
   void  checkBinding( const Camera& camera,
                       const Scene&  scene,
                       int32         width,
                       int32         height )                         const;


/// fields ---------------------------------------------------------------------
private:
   const Camera*                 pCamera_m;
   const Scene*                  pScene_m;
   int32                         width_m;
   int32                         height_m;
   int32                         maxDepth_m;
   int32u                        seed_m;

   int32                         passes_m;
   int32                         lastSuffixCount_m;
   std::int64_t                  totalSuffixCount_m;
   int32                         lastValidSourceCount_m;
   int32                         densityErrorCount_m;
   mutable int32                 occludedConnectionCount_m;
   real64                        sumDensity_m;
   real64                        minPositiveDensity_m;
   real64                        maxDensity_m;

   std::vector<FirstHit>         firstHits_m;
   std::vector<Candidate>        candidates_m;
   std::vector<Vector3f>         linearImage_m;
   std::vector<Vector3f>         passColors_m;
};


}//namespace


#endif//__cplusplus


#endif//PathReuse_h
