// Windowless ordered-query example: front-to-back alpha compositing of a
// stack of translucent, intersecting triangles, including two faces at the
// exact same depth. The scene is never sorted; OrderedHitQuery hands out the
// hits in (t, inst, prim) order and the renderer stops once the remaining
// transmittance drops below 1%.
//
// Usage: ordered_layers <output.ppm>
// Writes a 32x32 ASCII (P3) PPM.

#define TINYBVH_IMPLEMENTATION
#include "tiny_bvh.h"
#include "tiny_bvh_ordered.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

using namespace tinybvh;

namespace {

struct Material { float r, g, b, alpha; };

constexpr int IMG = 32;
constexpr float STOP_TRANSMITTANCE = 0.01f;

} // namespace

int main( int argc, char** argv )
{
	if (argc != 2) { std::fprintf( stderr, "usage: %s <output.ppm>\n", argv[0] ); return 2; }

	// Triangle soup. The first two faces are coincident (same depth, different
	// color and winding content); the next two are tilted and cross each other;
	// the rest are parallel translucent layers. Nothing here is sorted.
	std::vector<bvhvec4> tris;
	auto add = [&]( bvhvec4 a, bvhvec4 b, bvhvec4 c ) {
		tris.push_back( a ), tris.push_back( b ), tris.push_back( c );
	};
	// Equal-depth pair at z = 2.
	add( { -1.3f, -1.3f, 2, 0 }, { 1.3f, -1.3f, 2, 0 }, { 0, 1.3f, 2, 0 } );
	add( { 0, 1.3f, 2, 0 }, { 1.3f, -1.3f, 2, 0 }, { -1.3f, -1.3f, 2, 0 } );
	// Two tilted, mutually intersecting sheets.
	add( { -1.4f, -1.4f, 3, 0 }, { 1.4f, -1.4f, 3, 0 }, { 0, 1.4f, 7, 0 } );
	add( { -1.4f, 1.4f, 8, 0 }, { 1.4f, 1.4f, 8, 0 }, { 0, -1.4f, 4, 0 } );
	// Parallel layers, enough to drive transmittance below the threshold.
	for (int i = 0; i < 10; i++)
	{
		const float z = 10.0f + 2.0f * i;
		add( { -1.25f, -1.25f, z, 0 }, { 1.25f, -1.25f, z, 0 }, { 0, 1.25f, z, 0 } );
	}
	const uint32_t triCount = (uint32_t)(tris.size() / 3);

	const Material materials[] = {
		{ 0.95f, 0.15f, 0.15f, 0.45f }, // red,     shared depth
		{ 0.15f, 0.85f, 0.25f, 0.45f }, // green,   shared depth
		{ 0.15f, 0.35f, 0.95f, 0.40f }, // blue,    tilted
		{ 0.95f, 0.80f, 0.15f, 0.40f }, // yellow,  tilted
	};
	std::vector<Material> mats( triCount );
	for (uint32_t i = 0; i < triCount; i++)
		mats[i] = (i < 4) ? materials[i] : Material{ 0.55f, 0.45f, 0.85f, 0.40f };

	BVH bvh;
	bvh.Build( tris.data(), triCount );

	const float bg[3] = { 0.04f, 0.05f, 0.09f };
	uint8_t image[IMG * IMG * 3];

	for (int py = 0; py < IMG; py++) for (int px = 0; px < IMG; px++)
	{
		// Orthographic-style primary rays over [-2, 2]^2.
		const float x = -2.0f + 4.0f * (px + 0.5f) / IMG;
		const float y = 2.0f - 4.0f * (py + 0.5f) / IMG;
		const Ray ray( bvhvec3( x, y, -5.0f ), bvhvec3( 0.0f, 0.0f, 2.0f ), BVH_FAR );

		float color[3] = { bg[0], bg[1], bg[2] };
		float transmittance = 1.0f;
		OrderedHitQuery query( bvh, ray, 0.0f );
		OrderedHit hit;
		// Front-to-back "over" compositing; bail out once nothing visible can
		// contribute. The query is simply abandoned when we stop early.
		while (transmittance >= STOP_TRANSMITTANCE && query.Next( hit ))
		{
			const Material& m = mats[hit.prim];
			color[0] += transmittance * m.alpha * m.r;
			color[1] += transmittance * m.alpha * m.g;
			color[2] += transmittance * m.alpha * m.b;
			transmittance *= (1.0f - m.alpha);
		}
		const int idx = (py * IMG + px) * 3;
		for (int k = 0; k < 3; k++)
		{
			float v = color[k];
			if (v < 0.0f) v = 0.0f;
			if (v > 1.0f) v = 1.0f;
			image[idx + k] = (uint8_t)(v * 255.0f + 0.5f);
		}
	}

	std::FILE* f = std::fopen( argv[1], "wb" );
	if (!f) { std::fprintf( stderr, "cannot open %s\n", argv[1] ); return 1; }
	std::fprintf( f, "P3\n%d %d\n255\n", IMG, IMG );
	for (int p = 0; p < IMG * IMG; p++)
		std::fprintf( f, "%d %d %d\n", image[p * 3], image[p * 3 + 1], image[p * 3 + 2] );
	std::fclose( f );
	return 0;
}
