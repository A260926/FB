// Shared scene description for examples/ordered_layers.cpp and the ordered
// query tests. Deliberately free of TinyBVH types: the example and the tests
// each convert the plain floats to their own vertex representation.
#pragma once

#include <vector>

namespace ordered_layers_demo {

inline constexpr int IMG_SIZE = 32;
// Background color in 0..255 PPM space; also checked by the render tests.
inline constexpr float BG_R = 10.0f, BG_G = 12.0f, BG_B = 24.0f;
// Transmittance at which the front-to-back composite stops.
inline constexpr float CUTOFF = 0.01f;

struct Layer
{
	float v[3][3]; // three triangle vertices, xyz each
	float r, g, b, alpha;
};

// Deterministic scene: tilted, mutually interpenetrating translucent layers,
// an exactly coincident equal-depth pair, and enough rear layers that central
// rays reach the transmittance cutoff.
inline std::vector<Layer> BuildScene()
{
	std::vector<Layer> L;
	auto add = [&]( float ax, float ay, float az, float bx, float by, float bz,
		float cx, float cy, float cz, float r, float g, float b, float alpha )
	{
		Layer t{ { { ax, ay, az }, { bx, by, bz }, { cx, cy, cz } }, r, g, b, alpha };
		L.push_back( t );
	};
	// Wide tilted red sheet, crossing the z=4 plane on one side.
	add( -1.2f, -1.0f, 2.6f, 1.2f, -1.0f, 3.4f, 0.0f, 1.2f, 3.0f, 0.90f, 0.18f, 0.16f, 0.45f );
	// Green sheet tilted the other way: the two sheets interpenetrate.
	add( -1.1f, -0.9f, 3.6f, 1.1f, -0.9f, 2.8f, 0.1f, 1.1f, 3.3f, 0.16f, 0.80f, 0.24f, 0.45f );
	// Exactly coincident equal-depth pair at z = 4, different colors.
	add( -0.9f, -0.9f, 4.0f, 0.9f, -0.9f, 4.0f, 0.0f, 0.9f, 4.0f, 0.95f, 0.75f, 0.15f, 0.50f );
	add( -0.9f, -0.9f, 4.0f, 0.9f, -0.9f, 4.0f, 0.0f, 0.9f, 4.0f, 0.20f, 0.55f, 0.95f, 0.50f );
	// Diagonal yellow sliver cutting through several layers.
	add( -0.25f, -1.1f, 2.4f, 0.25f, -1.1f, 2.4f, 0.25f, 1.1f, 6.4f, 0.95f, 0.85f, 0.25f, 0.40f );
	// Broad rear sheets so central rays actually reach the 1% cutoff.
	add( -1.0f, -1.0f, 5.2f, 1.0f, -1.0f, 5.2f, 0.0f, 1.0f, 5.2f, 0.55f, 0.30f, 0.85f, 0.50f );
	add( -1.0f, -1.0f, 6.2f, 1.0f, -1.0f, 6.2f, 0.0f, 1.0f, 6.2f, 0.25f, 0.75f, 0.70f, 0.50f );
	add( -0.8f, -0.8f, 7.2f, 0.8f, -0.8f, 7.2f, 0.0f, 0.8f, 7.2f, 0.80f, 0.40f, 0.60f, 0.50f );
	// Deep rear sheets: dense-center rays cross the transmittance cutoff while
	// these layers are still pending on the ordered query.
	add( -1.0f, -1.0f, 8.2f, 1.0f, -1.0f, 8.2f, 0.0f, 1.0f, 8.2f, 0.45f, 0.55f, 0.80f, 0.50f );
	add( -1.0f, -1.0f, 9.2f, 1.0f, -1.0f, 9.2f, 0.0f, 1.0f, 9.2f, 0.60f, 0.50f, 0.35f, 0.50f );
	return L;
}

} // namespace ordered_layers_demo
