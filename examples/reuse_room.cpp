/*------------------------------------------------------------------------------

   MiniLight C++ : minimal global illumination renderer

   Diffuse suffix-reconnection example.

   Takes a single output directory, builds an enclosed black-background room,
   drives the real Camera through PathReuse, and writes:
     reuse.ppm  - tone mapped image (the original Image PPM path)
     linear.txt - per-pixel linear accumulated colour
     stats.txt  - pass count and the real suffix trace count, proving each
                  valid source produces exactly one suffix per pass

   Usage: reuse-example <output-directory>
------------------------------------------------------------------------------*/


#include <sstream>
#include <fstream>
#include <iostream>
#include <string>
#include <cstdio>

#include "Camera.hpp"
#include "Scene.hpp"
#include "Image.hpp"
#include "Random.hpp"
#include "PathReuse.hpp"


using namespace hxa7241_minilight;
using namespace hxa7241_general;


namespace
{
   // Fully enclosed box [-3,3] x [-2,2] x [1,5], black sky/ground. The two
   // ceiling triangles are wound with downward normals and carry the light.
   const char ROOM_SCENE[] =
      "(0 0 0) (0 0 0)\n"
      // floor y = -2
      "(-3 -2 1) (-3 -2 5) (3 -2 1)  (0.7 0.7 0.7) (0 0 0)\n"
      "(3 -2 1) (-3 -2 5) (3 -2 5)   (0.7 0.7 0.7) (0 0 0)\n"
      // ceiling y = 2, normals face down into the room and emit
      "(-3 2 1) (3 2 1) (-3 2 5)     (0.8 0.8 0.8) (6 6 6)\n"
      "(3 2 1) (3 2 5) (-3 2 5)      (0.8 0.8 0.8) (6 6 6)\n"
      // back wall z = 5
      "(-3 -2 5) (3 -2 5) (-3 2 5)   (0.7 0.7 0.7) (0 0 0)\n"
      "(3 -2 5) (3 2 5) (-3 2 5)     (0.7 0.7 0.7) (0 0 0)\n"
      // front wall z = 1 (behind the camera)
      "(-3 -2 1) (-3 2 1) (3 -2 1)   (0.7 0.7 0.7) (0 0 0)\n"
      "(3 -2 1) (-3 2 1) (3 2 1)     (0.7 0.7 0.7) (0 0 0)\n"
      // left wall x = -3
      "(-3 -2 1) (-3 -2 5) (-3 2 1)  (0.6 0.2 0.2) (0 0 0)\n"
      "(-3 2 1) (-3 -2 5) (-3 2 5)   (0.6 0.2 0.2) (0 0 0)\n"
      // right wall x = 3
      "(3 -2 1) (3 2 1) (3 -2 5)     (0.2 0.2 0.6) (0 0 0)\n"
      "(3 2 1) (3 2 5) (3 -2 5)      (0.2 0.2 0.6) (0 0 0)\n";
}


int main
(
   int   argc,
   char* argv[]
)
{
   if( argc != 2 )
   {
      std::cerr << "usage: " << argv[0] << " <output-directory>\n";
      return 2;
   }
   const std::string outDir( argv[1] );

   const int32 width  = 48;
   const int32 height = 36;
   const int32 depthD = 4;
   const int32 passes = 3;

   std::istringstream ci( "(0 0 2) (0 0 1) 90" );
   const Camera camera( ci );

   std::istringstream si( ROOM_SCENE );
   si.exceptions( std::ios::badbit | std::ios::failbit | std::ios::eofbit );
   const Scene scene( si, camera.getEyePoint() );

   std::istringstream ii( "48 36" );
   Image image( ii );

   PathReuse reuse;
   reuse.bind( camera, scene, width, height, depthD, 12345u );

   for( int32 p = 0; p < passes; ++p )
   {
      if( !reuse.runPass( camera, scene, image ) )
      {
         std::cerr << "reuse pass " << p << " failed a density check\n";
         return 3;
      }
   }

   // the room is fully enclosed: every valid source's single cosine edge must
   // land on a surface, so exactly one real suffix is traced per valid source
   const int32 validSources = reuse.getLastValidSourceCount();
   const int32 lastSuffixes = reuse.getLastSuffixCount();
   if( (validSources != width * height) | (lastSuffixes != validSources) )
   {
      std::cerr << "suffix/source invariant broken: sources=" << validSources
                << " suffixes=" << lastSuffixes << "\n";
      return 4;
   }

   // PPM through the original tone-mapping formatter
   {
      std::ofstream ppm( (outDir + "/reuse.ppm").c_str(), std::ios::binary );
      if( !ppm ) return 5;
      image.getFormatted( ppm, reuse.getCompletedPasses() );
   }

   // linear accumulation, never the display image
   {
      std::ofstream linear( (outDir + "/linear.txt").c_str() );
      if( !linear ) return 6;
      for( int32 y = 0; y < height; ++y )
         for( int32 x = 0; x < width; ++x )
         {
            const Vector3f c( reuse.getLinearPixel( x, y ) );
            linear << x << ' ' << y << ' ' << c[0] << ' ' << c[1] << ' '
                   << c[2] << '\n';
         }
   }

   // stats and the one-suffix-per-source proof
   {
      std::ofstream stats( (outDir + "/stats.txt").c_str() );
      if( !stats ) return 7;

      real64 sumDensity = 0.0, minDensity = 0.0, maxDensity = 0.0;
      int32  zeroDensity = 0, occluded = 0;
      reuse.getDiagnostics( sumDensity, minDensity, maxDensity, zeroDensity,
         occluded );

      stats << "completed_passes "      << reuse.getCompletedPasses() << '\n';
      stats << "image "                 << width << ' ' << height << '\n';
      stats << "max_depth_D "           << reuse.getMaxDepth() << '\n';
      stats << "valid_sources_last_pass " << validSources << '\n';
      stats << "suffixes_last_pass "    << lastSuffixes << '\n';
      stats << "suffixes_total "        << reuse.getTotalSuffixCount() << '\n';
      stats << "suffixes_per_valid_source_per_pass 1\n";
      stats << "density_zero_candidates " << zeroDensity << '\n';
      stats << "occluded_connections "  << occluded << '\n';
      stats << "density_sum "           << sumDensity << '\n';
      stats << "density_min_positive "  << minDensity << '\n';
      stats << "density_max "           << maxDensity << '\n';
   }

   std::printf(
      "reuse_room passes=%d sources=%d suffixes_last=%d suffixes_total=%lld\n",
      static_cast<int>( reuse.getCompletedPasses() ),
      static_cast<int>( validSources ),
      static_cast<int>( lastSuffixes ),
      static_cast<long long>( reuse.getTotalSuffixCount() ) );

   return 0;
}
