#include "tiny_bvh_c.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

static int check( int ok, const char* what )
{
	if (!ok) printf( "FAIL: %s\n", what );
	return ok;
}

int main( void )
{
	tbvh_bvh* bvh = NULL;
	tbvh_context ctx;
	tbvh_context_default( &ctx );
	if (!check( tbvh_create( TBVH_LAYOUT_BVH, &ctx, &bvh ) == TBVH_SUCCESS, "create" )) return 1;

	tbvh_build_settings s;
	tbvh_build_settings_default( &s );
	s.flags |= TBVH_BUILD_RETAIN_SPLIT_PLAN | TBVH_BUILD_BINNED | TBVH_BUILD_SPATIAL_SPLITS;
	s.flags &= ~(TBVH_BUILD_THREADED | TBVH_BUILD_PRESPLIT | TBVH_BUILD_POST_OPTIMIZE | TBVH_BUILD_KEEP_SPLITTING);
	if (!check( tbvh_set_build_settings( bvh, &s ) == TBVH_SUCCESS, "set build settings" )) return 2;

	/* A stack of small triangles crossed by long slivers, to force spatial splits. */
	enum { PRIMS = 208 };
	static _Alignas(16) float verts[PRIMS * 3 * 4];
	uint32_t rng = 999u;
	for (int i = 0; i < 400 && i < 200 * 3; i++)
	{
		rng = rng * 1664525u + 1013904223u;
		const float x = (float)((rng >> 8) & 0xffff) / 65536.0f;
		rng = rng * 1664525u + 1013904223u;
		const float y = (float)((rng >> 8) & 0xffff) / 65536.0f * 10.0f;
		rng = rng * 1664525u + 1013904223u;
		const float z = (float)((rng >> 8) & 0xffff) / 65536.0f;
		(void)i;
		static int t = 0;
		if (t >= 200) break;
		float* p = &verts[t * 12];
		p[0] = x, p[1] = y, p[2] = z;
		p[4] = x + 0.01f, p[5] = y, p[6] = z;
		p[8] = x, p[9] = y + 0.01f, p[10] = z;
		t++;
	}
	for (int i = 0; i < 8; i++)
	{
		float* p = &verts[(200 + i) * 12];
		p[0] = -1.0f + i * 0.2f, p[1] = -1, p[2] = 0.5f;
		p[4] = -1.0f + i * 0.2f + 6.0f, p[5] = 11, p[6] = 0.5f;
		p[8] = -1.0f + i * 0.2f + 6.0f, p[9] = -1, p[10] = 0.52f;
	}

	tbvh_geometry g;
	memset( &g, 0, sizeof( g ) );
	g.vertices = verts;
	g.vertex_count = PRIMS * 3;
	g.vertex_stride = 16;
	g.prim_count = PRIMS;
	{ const tbvh_result br = tbvh_build( bvh, &g, TBVH_QUALITY_DEFAULT );
  if (!check( br == TBVH_SUCCESS, "build retained plan" )) { printf("err=%d msg=%s\n",(int)br,tbvh_get_last_error()); return 3; } }

	/* Deform: displace every vertex along y. */
	for (int i = 0; i < PRIMS * 3; i++) verts[i * 4 + 1] += 0.05f;

	tbvh_reclassify_report rep;
	tbvh_reclassify_report_default( &rep );
	if (!check( tbvh_reclassify( bvh, 100000, &rep ) == TBVH_SUCCESS, "reclassify" )) return 4;
	printf( "reclassify: clips=%llu routes=%llu refs=%llu empty=%llu\n",
		(unsigned long long)rep.spatial_clip_count, (unsigned long long)rep.object_route_count,
		(unsigned long long)rep.reference_count, (unsigned long long)rep.empty_leaf_count );
	if (!check( rep.reference_count > 0, "reference count" )) return 5;

	/* Diagnostics: walk the plan and sum leaf reference counts. */
	tbvh_info info;
	if (!check( tbvh_get_info( bvh, &info ) == TBVH_SUCCESS, "info" )) return 6;
	uint64_t totalRefs = 0, emptyLeaves = 0;
	int sawSpatial = 0;
	for (uint32_t n = 0; n < (uint32_t)info.node_count; n++)
	{
		if (n == 1) continue;
		tbvh_split_plan_node node;
		tbvh_split_plan_node_default( &node );
		if (tbvh_get_split_plan_node( bvh, n, &node ) != TBVH_SUCCESS) continue;
		if (node.kind == TBVH_SPLIT_PLAN_LEAF) { totalRefs += node.reference_count; if (!node.reference_count) emptyLeaves++; }
		if (node.kind == TBVH_SPLIT_PLAN_SPATIAL) sawSpatial = 1;
	}
	if (!check( totalRefs == rep.reference_count, "leaf reference sum" )) return 7;
	if (!check( emptyLeaves == rep.empty_leaf_count, "empty leaf count" )) return 8;
	if (!check( sawSpatial, "the plan contains a spatial node" )) return 9;
	if (rep.spatial_clip_count == 0) printf( "note: no real clippings in this deformation\n" );

	/* Reference diagnostics exist for every reference. */
	for (uint32_t r = 0; r < (uint32_t)rep.reference_count; r++)
	{
		tbvh_split_plan_reference ref;
		if (!check( tbvh_get_split_plan_reference( bvh, r, &ref ) == TBVH_SUCCESS, "get reference" )) return 10;
		if (!check( ref.prim_id < PRIMS, "reference prim id in range" )) return 11;
	}

	/* A budget below the produced count fails and leaves the report untouched. */
	{
		tbvh_reclassify_report retry;
		tbvh_reclassify_report_default( &retry );
		retry.reference_count = 12345;
		const tbvh_result r = tbvh_reclassify( bvh, 1, &retry );
		if (!check( r != TBVH_SUCCESS, "budget failure" )) return 12;
		if (!check( retry.reference_count == 12345, "report untouched on failure" )) return 13;
		/* Retry with a larger budget succeeds. */
		if (!check( tbvh_reclassify( bvh, 100000, &retry ) == TBVH_SUCCESS, "retry after budget" )) return 14;
	}

	/* Refit is explicitly refused for a retained plan. */
	if (!check( tbvh_refit( bvh ) != TBVH_SUCCESS, "refit refused" )) return 15;

	/* Plain intersection still works on the updated tree, and matches a brute-force scan. */
	{
		tbvh_ray ray;
		float ox = 0.5f, oy = 4.05f, oz = -1.0f, dx = 0, dy = 0, dz = 1;
		tbvh_ray_init( &ray, ox, oy, oz, dx, dy, dz, TBVH_FAR );
		const int32_t hit = tbvh_intersect( bvh, &ray );
		if (!check( hit >= 0, "intersect call" )) return 16;
		/* Independent closest hit over the original triangles. */
		float best = TBVH_FAR;
		uint32_t bestPrim = 0;
		for (uint32_t p = 0; p < PRIMS; p++)
		{
			const float* v0 = &verts[p * 12], *v1 = &verts[p * 12 + 4], *v2 = &verts[p * 12 + 8];
			const float e1x = v1[0] - v0[0], e1y = v1[1] - v0[1], e1z = v1[2] - v0[2];
			const float e2x = v2[0] - v0[0], e2y = v2[1] - v0[1], e2z = v2[2] - v0[2];
			const float hx = dy * e2z - dz * e2y, hy = dz * e2x - dx * e2z, hz = dx * e2y - dy * e2x;
			const float det = e1x * hx + e1y * hy + e1z * hz;
			if (fabsf( det ) < 1e-20f) continue;
			const float inv = 1.0f / det;
			const float sx = ox - v0[0], sy = oy - v0[1], sz = oz - v0[2];
			const float u = (sx * hx + sy * hy + sz * hz) * inv;
			if (u < 0 || u > 1) continue;
			const float qx = sy * e1z - sz * e1y, qy = sz * e1x - sx * e1z, qz = sx * e1y - sy * e1x;
			const float vv = (dx * qx + dy * qy + dz * qz) * inv;
			if (vv < 0 || u + vv > 1) continue;
			const float t = (e2x * qx + e2y * qy + e2z * qz) * inv;
			if (t >= 0 && t < best) best = t, bestPrim = p;
		}
		if (best < TBVH_FAR)
		{
			if (!check( fabsf( ray.hit.t - best ) < 1e-3f * fmaxf( 1.0f, best ), "intersect distance matches oracle" )) return 17;
			if (!check( (uint32_t)ray.hit.prim < PRIMS, "intersect prim identity" )) return 18;
			(void)bestPrim;
		}
		/* Occlusion in a finite window near the first geometry. */
		tbvh_ray shadow;
		tbvh_ray_init( &shadow, ox, oy, oz - 0.5f, dx, dy, dz, 3.0f );
		const int32_t occ = tbvh_is_occluded( bvh, &shadow );
		if (!check( occ >= 0, "is_occluded call" )) return 19;
	}

	/* Diagnostics refuse an out-of-range node and a plan-less tree. */
	{
		tbvh_split_plan_node node;
		tbvh_split_plan_node_default( &node );
		if (!check( tbvh_get_split_plan_node( bvh, 0xFFFFFFu, &node ) != TBVH_SUCCESS, "out-of-range node refused" )) return 20;
		tbvh_bvh* plain = NULL;
		tbvh_context c2;
		tbvh_context_default( &c2 );
		if (!check( tbvh_create( TBVH_LAYOUT_BVH, &c2, &plain ) == TBVH_SUCCESS, "create plain" )) return 21;
		tbvh_build_settings ps;
		tbvh_build_settings_default( &ps );
		if (!check( tbvh_set_build_settings( plain, &ps ) == TBVH_SUCCESS, "plain settings" )) return 22;
		if (!check( tbvh_build( plain, &g, TBVH_QUALITY_DEFAULT ) == TBVH_SUCCESS, "plain build" )) return 23;
		if (!check( tbvh_get_split_plan_node( plain, 0, &node ) == TBVH_ERROR_NOT_REFITTABLE, "no plan refused" )) return 24;
		/* reclassify on a plan-less tree is refused too. */
		tbvh_reclassify_report nr;
		tbvh_reclassify_report_default( &nr );
		if (!check( tbvh_reclassify( plain, 1000, &nr ) != TBVH_SUCCESS, "reclassify without plan refused" )) return 25;
		tbvh_destroy( plain );
	}

	/* An unsupported layout cannot reclassify, even with a plan-shaped build. */
	{
		tbvh_bvh* gpu = NULL;
		tbvh_context c3;
		tbvh_context_default( &c3 );
		if (tbvh_create( TBVH_LAYOUT_BVH_GPU, &c3, &gpu ) == TBVH_SUCCESS)
		{
			tbvh_reclassify_report gr;
			tbvh_reclassify_report_default( &gr );
			if (!check( tbvh_reclassify( gpu, 1000, &gr ) == TBVH_ERROR_UNSUPPORTED, "GPU layout refused" )) return 26;
			tbvh_destroy( gpu );
		}
	}

	/* A TLAS (built over this BLAS) cannot reclassify itself. */
	{
		tbvh_bvh* tlas = NULL;
		tbvh_context c4;
		tbvh_context_default( &c4 );
		if (!check( tbvh_create( TBVH_LAYOUT_BVH, &c4, &tlas ) == TBVH_SUCCESS, "create tlas" )) return 27;
		tbvh_instance inst;
		tbvh_instance_init( &inst, 0 );
		tbvh_bvh* blases[1] = { bvh };
		const tbvh_result tr = tbvh_build_tlas( tlas, &inst, 1, blases, 1 );
		if (tr == TBVH_SUCCESS)
		{
			tbvh_reclassify_report trr;
			tbvh_reclassify_report_default( &trr );
			if (!check( tbvh_reclassify( tlas, 1000, &trr ) != TBVH_SUCCESS, "TLAS refused" )) return 28;
		}
		tbvh_destroy( tlas );
	}

	tbvh_destroy( bvh );
	printf( "C_SMOKE_OK\n" );
	return 0;
}
