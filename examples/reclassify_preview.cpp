// reclassify_preview.cpp - animated-slender-mesh preview using a retained split plan.
//
// Builds a BLAS with BVHBuildSettings::retainSplitPlan, deforms its vertices over three frames, and
// updates the tree in place with BVH::Reclassify. Every frame is traced with the ordinary
// BVH::Intersect / BVH::IsOccluded, then shaded, producing a 3-tile 64x24 P3 PPM image.
//
// Usage: reclassify_preview <output.ppm>

#include "tiny_bvh.h"
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <cstring>
#include <vector>

using namespace tinybvh;

static constexpr int TILE_W = 64, TILE_H = 24, TILES = 3;
static constexpr int FRAMES = 3;

// Deterministic 0..1 noise.
static uint32_t g_rng = 0x12345678u;
static float rnd()
{
	g_rng = g_rng * 1664525u + 1013904223u;
	return (float)(g_rng >> 8) / 16777216.0f;
}

// The deforming mesh: a dense stack of small triangles plus a few long slivers that cross the whole
// stack. The slivers force spatial splits into the retained plan; when the mesh deforms, references
// move between leaves, and some leaves empty and refill.
struct Scene
{
	std::vector<bvhvec4> verts;	// three vertices per triangle, w unused
	int primCount = 0;
};

static void BuildScene( Scene& s )
{
	uint32_t saved = g_rng;
	// A curtain of thin vertical ribbons: each ribbon is two long, thin triangles spanning the full
	// height, with a slowly varying z so that neighbouring ribbons sit at slightly different depths.
	for (int i = 0; i < 64; i++)
	{
		const float x = -1.0f + i * 0.1f;
		const float z = 0.2f + 0.4f * (float)((i * 37) % 11) / 11.0f;
		const float w = 0.035f;
		s.verts.push_back( bvhvec4( x, -1.0f, z, 0 ) );
		s.verts.push_back( bvhvec4( x + w, 11.0f, z + 0.05f, 0 ) );
		s.verts.push_back( bvhvec4( x, 11.0f, z, 0 ) );
		s.verts.push_back( bvhvec4( x, -1.0f, z, 0 ) );
		s.verts.push_back( bvhvec4( x + w, -1.0f, z + 0.05f, 0 ) );
		s.verts.push_back( bvhvec4( x + w, 11.0f, z + 0.05f, 0 ) );
	}
	// A dense cluster of small triangles near the top, so a few leaves hold many references that
	// migrate when the mesh deforms.
	for (int i = 0; i < 200; i++)
	{
		const float x = rnd() * 3.0f, y = 7.0f + rnd() * 3.0f, z = 0.1f + rnd() * 0.6f;
		s.verts.push_back( bvhvec4( x, y, z, 0 ) );
		s.verts.push_back( bvhvec4( x + 0.03f, y, z, 0 ) );
		s.verts.push_back( bvhvec4( x, y + 0.03f, z, 0 ) );
	}
	// A few long diagonal slivers across the whole curtain: these force spatial splits.
	for (int i = 0; i < 8; i++)
	{
		const float x0 = -1.0f + i * 0.9f;
		s.verts.push_back( bvhvec4( x0, -1.0f, 0.5f, 0 ) );
		s.verts.push_back( bvhvec4( x0 + 6.4f, 11.0f, 0.5f, 0 ) );
		s.verts.push_back( bvhvec4( x0 + 6.4f, -1.0f, 0.52f, 0 ) );
	}
	s.primCount = (int)(s.verts.size() / 3);
	g_rng = saved;
}

// Deform the vertices in place. Frame 0 is the rest pose; later frames ripple and sway the mesh, so
// that references move between leaves and a few leaves empty out and refill.
static void Deform( Scene& s, const int frame )
{
	const float phase = (float)frame * 1.7f;
	for (size_t i = 0; i < s.verts.size(); i++)
	{
		bvhvec4& v = s.verts[i];
		const float wave = sinf( phase + v.z * 6.2831853f + v.x * 3.1415926f );
		v.y += 0.45f * wave;		// vertical ripple
		v.x += 0.18f * cosf( phase * 0.7f + v.y );	// lateral sway
		v.z += 0.10f * sinf( phase + v.x * 4.0f );	// depth wobble
	}
}

// Closest hit through a triangle soup (no acceleration structure) for shading the visible surface.
static bvhvec3 ShadeBackground( const int x, const int y, const int w, const int h )
{
	const float u = (x + 0.5f) / (float)w, v = (y + 0.5f) / (float)h;
	return bvhvec3( 0.05f + 0.10f * u, 0.06f + 0.12f * v, 0.12f + 0.18f * (1.0f - u * v) );
}

int main( int argc, char** argv )
{
	if (argc < 2) { fprintf( stderr, "usage: %s <output.ppm>\n", argc > 0 ? argv[0] : "reclassify_preview" ); return 2; }
	const char* path = argv[1];

	// Geometry lives in the caller's buffer, which is only ever deformed in place.
	Scene scene;
	BuildScene( scene );

	BVH bvh;
	bvh.settings.binnedBVH = true;
	bvh.settings.useSpatialSplits = true;
	bvh.settings.retainSplitPlan = true;
	bvh.settings.enableThreading = false;
	bvh.settings.useSIMDifavailable = false;
	bvh.Build( scene.verts.data(), scene.primCount );
	if (!bvh.hasSplitPlan()) { fprintf( stderr, "failed to build a retained split plan\n" ); return 3; }
	// The diagnostics read the plan that the build committed: leaf ranges, split kinds and planes.
	int spatialNodes = 0;
	for (uint32_t n = 0; n < bvh.usedNodes; n++)
	{
		if (n == 1) continue;
		SplitPlanNodeInfo info;
		if (bvh.GetSplitPlanNode( n, info ) && info.kind == SPLIT_PLAN_SPATIAL) spatialNodes++;
	}

	// A camera looking along +z from before the mesh.
	const bvhvec3 O( 2.2f, 5.0f, -18.0f );
	const float fov = 0.36f, aspect = 0.62f;
	std::vector<unsigned char> image( TILE_W * TILES * TILE_H * 3 );
	uint64_t totalRefs = 0, totalClips = 0, totalEmpty = 0;

	for (int frame = 0; frame < FRAMES; frame++)
	{
		if (frame > 0) Deform( scene, frame );
		ReclassifyReport rep;
		if (!bvh.Reclassify( 1u << 20, rep )) { fprintf( stderr, "reclassify failed on frame %d\n", frame ); return 4; }
		totalRefs += rep.referenceCount, totalClips += rep.spatialClipCount, totalEmpty += rep.emptyLeafCount;

		for (int ty = 0; ty < TILE_H; ty++) for (int tx = 0; tx < TILE_W; tx++)
		{
			const int px = frame * TILE_W + tx;
			const float sx = (2.0f * (tx + 0.5f) / TILE_W - 1.0f) * fov * aspect;
			const float sy = (1.0f - 2.0f * (ty + 0.5f) / TILE_H) * fov;
			const bvhvec3 D = tinybvh_normalize( bvhvec3( sx, sy, 1.0f ) );
			bvhvec3 color = ShadeBackground( px, ty, TILE_W * TILES, TILE_H );
			Ray ray( O, D );
			bvh.Intersect( ray );
			if (ray.hit.t < BVH_FAR)
			{
				// Diffuse-ish shading from the hit normal; the normal comes from the original triangle.
				const uint32_t prim = (uint32_t)ray.hit.prim;
				color = bvhvec3( 0.95f, 0.85f, 0.75f ); // unshaded fallback; visible on its own
				if (prim < (uint32_t)scene.primCount)
				{
					const bvhvec4* v = &scene.verts[prim * 3];
					const bvhvec3 n = tinybvh_normalize( tinybvh_cross( bvhvec3( v[1] ) - bvhvec3( v[0] ), bvhvec3( v[2] ) - bvhvec3( v[0] ) ) );
					const float lambert = 0.45f + 0.55f * fabsf( n.y );
					const float tint = 0.45f + 0.55f * (float)(ray.hit.u + ray.hit.v * 0.5f);
					color = bvhvec3( 0.95f * lambert * tint, 0.80f * lambert, 0.65f * lambert * (1.0f - tint) );
				}
				// A shadow term: cast a second, slightly offset ray toward the light.
				Ray shadow( bvhvec3( O ) + D * ray.hit.t + bvhvec3( 0, 0.02f, 0 ), tinybvh_normalize( bvhvec3( 0.3f, 1.0f, 0.2f ) ), 6.0f );
				if (bvh.IsOccluded( shadow )) color = color * 0.55f;
			}
			const uint32_t tile = frame * TILE_W + tx;
			const size_t ofs = ((size_t)ty * (TILE_W * TILES) + tile) * 3;
			for (int c = 0; c < 3; c++)
			{
				int ch = (int)(255.0f * color[c]);
				if (ch < 0) ch = 0;
				if (ch > 255) ch = 255;
				image[ofs + c] = (unsigned char)ch;
			}
		}
	}

	FILE* f = fopen( path, "wb" );
	if (!f) { fprintf( stderr, "cannot write '%s'\n", path ); return 5; }
	fprintf( f, "P3\n%d %d\n255\n", TILE_W * TILES, TILE_H );
	for (int y = 0; y < TILE_H; y++) for (int x = 0; x < TILE_W * TILES; x++)
	{
		const size_t ofs = ((size_t)y * (TILE_W * TILES) + x) * 3;
		fprintf( f, "%d %d %d%s", image[ofs], image[ofs + 1], image[ofs + 2], x + 1 == TILE_W * TILES ? "\n" : " " );
	}
	fclose( f );
	printf( "WROTE %s frames=%d spatial_nodes=%d refs=%llu clips=%llu empty_leaf_sum=%llu\n",
		path, FRAMES, spatialNodes, (unsigned long long)totalRefs, (unsigned long long)totalClips, (unsigned long long)totalEmpty );
	return 0;
}
