// ordered_layers.cpp: windowless offline-style use of tinybvh::OrderedHitQuery.
//
// Builds a small scene of colored, mutually interpenetrating translucent
// triangles, including an exactly coincident (equal-depth) pair, and renders
// a 32x32 ASCII P3 PPM. Each pixel is composited front-to-back straight from
// the ordered query: the scene triangles are never sorted up front. When the
// transmittance drops below 1% the query is paused and the remaining hits are
// simply never requested.
//
// Usage: ordered_layers <output.ppm>

#define TINYBVH_IMPLEMENTATION
#include "tiny_bvh.h"
#include "tiny_bvh_ordered.h"

#include <cstdint>
#include <cstdio>
#include <vector>

#include "ordered_layers_scene.h"

using namespace tinybvh;

namespace {

struct Pixel { float r = ordered_layers_demo::BG_R, g = ordered_layers_demo::BG_G, b = ordered_layers_demo::BG_B; };

} // namespace

int main( int argc, char** argv )
{
	if (argc != 2) { std::fprintf( stderr, "usage: %s <output.ppm>\n", argv[0] ); return 2; }
	using namespace ordered_layers_demo;
	const std::vector<Layer> layers = BuildScene();
	const uint32_t triCount = (uint32_t)layers.size();
	std::vector<bvhvec4> verts( triCount * 3 );
	for (uint32_t i = 0; i < triCount; i++) for (int k = 0; k < 3; k++)
		verts[i * 3 + k] = bvhvec4( layers[i].v[k][0], layers[i].v[k][1], layers[i].v[k][2], 0 );

	BVH bvh;
	bvh.Build( verts.data(), triCount );

	std::vector<Pixel> pixels( IMG_SIZE * IMG_SIZE );
	int stoppedEarly = 0;
	for (int py = 0; py < IMG_SIZE; py++) for (int px = 0; px < IMG_SIZE; px++)
	{
		// Ray through the pixel; direction intentionally left unnormalized.
		const float dx = ( ( px + 0.5f ) / IMG_SIZE * 2.0f - 1.0f ) * 0.9f;
		const float dy = ( ( py + 0.5f ) / IMG_SIZE * 2.0f - 1.0f ) * 0.9f;
		Ray ray( bvhvec3( 0, 0, 0 ), bvhvec3( dx, dy, 1.0f ) );
		OrderedHitQuery query( bvh, ray, 0.0f );
		Pixel& pixel = pixels[py * IMG_SIZE + px];
		float transmittance = 1.0f;
		OrderedHit hit;
		// Front-to-back alpha compositing, driven one hit at a time. No scene
		// sorting and no full hit collection takes place.
		while (transmittance >= CUTOFF && query.Next( hit ))
		{
			const Layer& L = layers[hit.prim];
			const float w = transmittance * L.alpha;
			pixel.r += w * L.r * 255.0f;
			pixel.g += w * L.g * 255.0f;
			pixel.b += w * L.b * 255.0f;
			transmittance *= ( 1.0f - L.alpha );
		}
		if (transmittance < CUTOFF) stoppedEarly++;
	}

	FILE* out = std::fopen( argv[1], "wb" );
	if (!out) { std::fprintf( stderr, "cannot open %s\n", argv[1] ); return 1; }
	std::fprintf( out, "P3\n%i %i\n255\n", IMG_SIZE, IMG_SIZE );
	for (int py = 0; py < IMG_SIZE; py++) for (int px = 0; px < IMG_SIZE; px++)
	{
		const Pixel& p = pixels[py * IMG_SIZE + px];
		auto clamp8 = []( float v ) { int x = (int)( v + 0.5f ); return v < 0 ? 0 : ( x > 255 ? 255 : x ); };
		std::fprintf( out, "%i %i %i\n", clamp8( p.r ), clamp8( p.g ), clamp8( p.b ) );
	}
	std::fclose( out );
	std::printf( "ordered_layers: wrote %s (%i pixels, %i pixels reached the 1%% cutoff)\n",
		argv[1], IMG_SIZE * IMG_SIZE, stoppedEarly );
	return 0;
}
