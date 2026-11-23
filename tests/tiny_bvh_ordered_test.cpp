// Tests for tinybvh::OrderedHitQuery / OrderedHitQueryEx.
//
// No main(): task_support/driver.cpp supplies it and drives the cases via
// register_ordered_tests(). Every expected result below is produced by an
// independent world-space triangle enumeration plus a sort; the reference
// never calls an ordered query or BVH::Intersect.

#define TINYBVH_IMPLEMENTATION
#include "tiny_bvh.h"
#include "tiny_bvh_ordered.h"

#include "../examples/ordered_layers_scene.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

#include "../task_support/registry.hpp"

using namespace tinybvh;

namespace {

// Relative-plus-absolute comparison; tolerances follow the task specification.
template <typename F> bool close( const F a, const F b, const F tol )
{
	return std::fabs( a - b ) <= tol + tol * std::fabs( b );
}
constexpr float FTOL = 2e-5f;
constexpr double DTOL = 1e-10;

template <typename F, typename V>
struct RefHit
{
	F t, u, v;
	uint32_t prim, inst;
};

// Independent double-sided Moeller-Trumbore test; zero-area triangles miss.
// V is the ray vector type; W is the stored vertex type (bvhvec4 for floats).
template <typename F, typename V, typename W>
bool refTriangle( const V& O, const V& D, const W& a, const W& b, const W& c,
	const F tmin, const F tmax, F& t, F& u, F& v )
{
	const V va( a ), vb( b ), vc( c );
	const V e1 = vb - va, e2 = vc - va;
	const V h = tinybvh_cross( D, e2 );
	const F det = tinybvh_dot( e1, h );
	if (det == F( 0 )) return false;
	const F f = F( 1 ) / det;
	const V s = O - va;
	u = f * tinybvh_dot( s, h );
	if (!(u >= F( 0 ) && u <= F( 1 ))) return false;
	const V q = tinybvh_cross( s, e1 );
	v = f * tinybvh_dot( D, q );
	if (!(v >= F( 0 ) && v <= F( 1 ) && u + v <= F( 1 ))) return false;
	t = f * tinybvh_dot( e2, q );
	return t >= tmin && t < tmax;
}

template <typename F, typename V>
void refSort( std::vector<RefHit<F, V>>& hits )
{
	std::sort( hits.begin(), hits.end(), []( const RefHit<F, V>& a, const RefHit<F, V>& b )
	{
		if (a.t != b.t) return a.t < b.t;
		if (a.inst != b.inst) return a.inst < b.inst;
		return a.prim < b.prim;
	} );
}

// ----------------------------------------------------------------------------
// Float BLAS helpers
// ----------------------------------------------------------------------------

struct TriScene
{
	std::vector<bvhvec4> verts; // 3 vertices per triangle, tightly packed
	uint32_t count() const { return (uint32_t)verts.size() / 3; }
};

std::vector<RefHit<float, bvhvec3>> referenceBLAS( const TriScene& scene, const Ray& ray, const float tmin )
{
	std::vector<RefHit<float, bvhvec3>> out;
	for (uint32_t p = 0; p < scene.count(); p++)
	{
		float t, u, v;
		if (refTriangle( ray.O, ray.D, scene.verts[p * 3], scene.verts[p * 3 + 1], scene.verts[p * 3 + 2],
			tmin, ray.hit.t, t, u, v ))
			out.push_back( { t, u, v, p, 0 } );
	}
	refSort( out );
	return out;
}

std::vector<OrderedHit> collect( OrderedHitQuery& q )
{
	std::vector<OrderedHit> got;
	OrderedHit h;
	while (q.Next( h )) got.push_back( h );
	return got;
}

void checkSequence( ordered_test::Case& c, const std::vector<OrderedHit>& got,
	const std::vector<RefHit<float, bvhvec3>>& ref, const char* what )
{
	c.check( got.size() == ref.size(), std::string( what ) + ": hit count" );
	const size_t n = std::min( got.size(), ref.size() );
	for (size_t i = 0; i < n; i++)
	{
		c.check( got[i].inst == ref[i].inst, std::string( what ) + ": inst order" );
		c.check( got[i].prim == ref[i].prim, std::string( what ) + ": prim order" );
		c.check( close( got[i].t, ref[i].t, FTOL ), std::string( what ) + ": t" );
		c.check( close( got[i].u, ref[i].u, FTOL ), std::string( what ) + ": u" );
		c.check( close( got[i].v, ref[i].v, FTOL ), std::string( what ) + ": v" );
	}
}

// A small scene with distinct depths, an exactly coincident pair, a reversed
// winding triangle and a zero-area triangle.
TriScene makeMixedScene()
{
	TriScene s;
	auto add = [&]( bvhvec4 a, bvhvec4 b, bvhvec4 c ) { s.verts.push_back( a ), s.verts.push_back( b ), s.verts.push_back( c ); };
	add( { -1, -1, 2, 0 }, { 1, -1, 2, 0 }, { 0, 1, 2, 0 } );   // 0: z=2
	add( { -1, -1, 2, 0 }, { 1, -1, 2, 0 }, { 0, 1, 2, 0 } );   // 1: coincident with 0
	add( { 0, 1, 3, 0 }, { 1, -1, 3, 0 }, { -1, -1, 3, 0 } );   // 2: z=3, reversed winding
	add( { -1, -1, 4, 0 }, { 1, -1, 4, 0 }, { 0, 1, 4, 0 } );   // 3: z=4
	add( { -1, -1, 2.5f, 0 }, { 1, -1, 2.5f, 0 }, { 1, -1, 2.5f, 0 } ); // 4: zero area
	add( { 5, 5, 2, 0 }, { 6, 5, 2, 0 }, { 5, 6, 2, 0 } );      // 5: never on the ray
	return s;
}

// ----------------------------------------------------------------------------
// order group
// ----------------------------------------------------------------------------

void orderBlasFull( ordered_test::Case& c )
{
	const TriScene scene = makeMixedScene();
	BVH bvh;
	bvh.Build( scene.verts.data(), scene.count() );
	Ray ray( bvhvec3( 0, 0, 0 ), bvhvec3( 0, 0, 2 ) );
	OrderedHitQuery q( bvh, ray, 0.0f );
	const auto got = collect( q );
	const auto ref = referenceBLAS( scene, ray, 0.0f );
	checkSequence( c, got, ref, "orderBlasFull" );
	// tri 4 is degenerate: 4 real hits (0,1 at t=1; 2 at t=1.5; 3 at t=2).
	c.check( got.size() == 4, "orderBlasFull: degenerate and miss excluded" );
	c.check( got[0].prim == 0 && got[1].prim == 1, "orderBlasFull: coincident pair identity order" );
	c.check( got[0].t == got[1].t, "orderBlasFull: coincident pair keeps exact equal t" );
	c.check( got[2].prim == 2 && got[3].prim == 3, "orderBlasFull: front-to-back order" );
	// Exhausted queries stay finished and leave the result object untouched.
	OrderedHit sentinel{ 123, 456, 789, 999, 888 };
	OrderedHit before = sentinel;
	c.check( !q.Next( sentinel ), "orderBlasFull: Next stays false" );
	c.check( sentinel.t == before.t && sentinel.prim == before.prim && sentinel.inst == before.inst,
		"orderBlasFull: false Next does not modify out" );
	const auto st = q.GetStats();
	c.check( st.emittedHits == 4, "orderBlasFull: emitted counter" );
	c.check( st.triangleTests >= 4 && st.triangleTests <= scene.count(), "orderBlasFull: test counter bounded" );
}

void orderExactTies( ordered_test::Case& c )
{
	// Four exactly coplanar triangles at z=5 and one nearer triangle at z=2.
	TriScene s;
	auto add = [&]( bvhvec4 a, bvhvec4 b, bvhvec4 cc ) { s.verts.push_back( a ), s.verts.push_back( b ), s.verts.push_back( cc ); };
	add( { -1, -1, 2, 0 }, { 1, -1, 2, 0 }, { 0, 1, 2, 0 } );
	for (int k = 0; k < 4; k++) add( { -1, -1, 5, 0 }, { 1, -1, 5, 0 }, { 0, 1, 5, 0 } );
	BVH bvh;
	bvh.Build( s.verts.data(), s.count() );
	Ray ray( bvhvec3( 0, 0, 0 ), bvhvec3( 0, 0, 1 ) );
	OrderedHitQuery q( bvh, ray, 0.0f );
	const auto got = collect( q );
	c.check( got.size() == 5, "orderExactTies: all five hits delivered" );
	c.check( got[0].prim == 0, "orderExactTies: nearest first" );
	for (int k = 1; k < 5; k++)
	{
		c.check( got[k].prim == (uint32_t)k, "orderExactTies: tie broken by prim id" );
		c.check( got[k].t == got[1].t, "orderExactTies: exact equal distance preserved" );
		c.check( got[k].t == 5.0f, "orderExactTies: analytic distance" );
	}
	// TLAS: two instances with identical transforms produce bit-equal t values.
	BVH blas;
	blas.Build( s.verts.data(), 1 ); // single triangle at z=2
	BLASInstance inst[3] = { BLASInstance( 0 ), BLASInstance( 0 ), BLASInstance( 0 ) };
	inst[0].transform[11] = 3.0f; // identical placement for inst 0 and 1
	inst[1].transform[11] = 3.0f;
	inst[2].transform[11] = 7.0f;
	BVHBase* blasses[1] = { &blas };
	BVH tlas;
	tlas.Build( inst, 3, blasses, 1 );
	Ray r2( bvhvec3( 0, 0, 0 ), bvhvec3( 0, 0, 1 ) );
	OrderedHitQuery q2( tlas, r2, 0.0f );
	const auto tgot = collect( q2 );
	c.check( tgot.size() == 3, "orderExactTies: one hit per instance" );
	c.check( tgot[0].inst == 0 && tgot[1].inst == 1 && tgot[2].inst == 2, "orderExactTies: instance id tie order" );
	c.check( tgot[0].t == tgot[1].t, "orderExactTies: shared-transform instances exactly equal" );
	c.check( tgot[0].prim == 0 && tgot[1].prim == 0, "orderExactTies: original prim id" );

	// Coincident at the hit point but with different triangle content: both are
	// distinct primitives and both must survive, ordered by prim id.
	TriScene cc;
	cc.verts = {
		{ -1, -1, 5, 0 }, { 1, -1, 5, 0 }, { 0, 1, 5, 0 },   // smaller triangle
		{ -2, -2, 5, 0 }, { 2, -2, 5, 0 }, { 0, 2, 5, 0 },   // larger, same hit point
		{ -1, -1, 2, 0 }, { 1, -1, 2, 0 }, { 0, 1, 2, 0 }    // nearer reference
	};
	BVH cbvh;
	cbvh.Build( cc.verts.data(), cc.count() );
	Ray cray( bvhvec3( 0, 0, 0 ), bvhvec3( 0, 0, 1 ) );
	OrderedHitQuery cq( cbvh, cray, 0.0f );
	const auto cgot = collect( cq );
	c.check( cgot.size() == 3, "orderExactTies: distinct-content coincident tris both kept" );
	c.check( cgot[0].prim == 2, "orderExactTies: nearer triangle first" );
	c.check( cgot[1].prim == 0 && cgot[2].prim == 1, "orderExactTies: coincident distinct tris by prim id" );
	c.check( cgot[1].t == cgot[2].t && cgot[1].t == 5.0f, "orderExactTies: coincident distinct tris equal t" );
}

void orderInterval( ordered_test::Case& c )
{
	TriScene s;
	auto add = [&]( float z ) { s.verts.push_back( { -1, -1, z, 0 } ), s.verts.push_back( { 1, -1, z, 0 } ), s.verts.push_back( { 0, 1, z, 0 } ); };
	add( 1 ), add( 2 ), add( 3 );
	BVH bvh;
	bvh.Build( s.verts.data(), s.count() );
	// tmin exactly on a hit is included; tmax exactly on a hit is excluded.
	Ray ray( bvhvec3( 0, 0, 0 ), bvhvec3( 0, 0, 1 ), 3.0f );
	OrderedHitQuery q( bvh, ray, 2.0f );
	auto got = collect( q );
	c.check( got.size() == 1 && got[0].prim == 1 && got[0].t == 2.0f, "orderInterval: half-open [tmin,tmax)" );
	// Empty interval yields an empty query.
	Ray ray2( bvhvec3( 0, 0, 0 ), bvhvec3( 0, 0, 1 ), 2.0f );
	OrderedHitQuery q2( bvh, ray2, 2.0f );
	c.check( collect( q2 ).empty(), "orderInterval: tmin == tmax is empty" );
	// Ray starting inside the scene bounds with positive tmin.
	Ray ray3( bvhvec3( 0, 0, 1.5f ), bvhvec3( 0, 0, 1 ), 5.0f );
	OrderedHitQuery q3( bvh, ray3, 0.5f ); // first allowed hit at world t=0.5 -> z=2
	got = collect( q3 );
	c.check( got.size() == 2 && got[0].prim == 1 && got[1].prim == 2, "orderInterval: inside-box origin" );
}

void orderAxisRays( ordered_test::Case& c )
{
	// Six axis-aligned plates around the origin, reached by rays with zero and
	// signed-zero direction components.
	TriScene s;
	auto plate = [&]( int axis, int side )
	{
		const int u = ( axis + 1 ) % 3, v = ( axis + 2 ) % 3;
		float q[3][3] = { { 0, 0, 0 }, { 0, 0, 0 }, { 0, 0, 0 } };
		for (int k = 0; k < 3; k++) q[k][axis] = side * 2.0f;
		q[0][u] = -1; q[0][v] = -1; q[1][u] = 1; q[1][v] = -1; q[2][u] = 0; q[2][v] = 1;
		for (auto& p : q) s.verts.push_back( { p[0], p[1], p[2], 0 } );
	};
	for (int axis = 0; axis < 3; axis++) for (int side = -1; side <= 1; side += 2) plate( axis, side );
	BVH bvh;
	bvh.Build( s.verts.data(), s.count() );
	const float comps[4] = { -1.0f, -0.0f, 0.0f, 1.0f };
	for (int mask = 0; mask < 64; mask++)
	{
		bvhvec3 D( comps[mask & 3], comps[( mask >> 2 ) & 3], comps[mask >> 4] );
		const int zeros = ( D.x == 0 ) + ( D.y == 0 ) + ( D.z == 0 );
		if (zeros == 0 || zeros == 3) continue;
		Ray ray( bvhvec3( 0.1f, -0.1f, 0.05f ), D );
		OrderedHitQuery q( bvh, ray, 0.0f );
		const auto got = collect( q );
		const auto ref = referenceBLAS( s, ray, 0.0f );
		checkSequence( c, got, ref, "orderAxisRays" );
	}
}

void orderEmptyAndDouble( ordered_test::Case& c )
{
	// Unbuilt BVH: empty query, zero counters.
	BVH empty;
	Ray ray( bvhvec3( 0, 0, 0 ), bvhvec3( 0, 0, 1 ) );
	{
		OrderedHitQuery q( empty, ray, 0.0f );
		OrderedHit h{ 1, 2, 3, 4, 5 };
		c.check( !q.Next( h ), "orderEmpty: no hit" );
		c.check( h.prim == 4 && h.inst == 5, "orderEmpty: out untouched" );
		const auto st = q.GetStats();
		c.check( st.nodeVisits == 0 && st.triangleTests == 0 && st.instanceEntries == 0 && st.emittedHits == 0,
			"orderEmpty: zero counters" );
	}
	// Double-precision path against a double-only reference.
	std::vector<bvhdbl3> d;
	const TriScene fs = makeMixedScene();
	for (const bvhvec4& v : fs.verts) d.push_back( bvhdbl3( v.x, v.y, v.z ) );
	BVH_Double bd;
	bd.Build( d.data(), fs.count() );
	RayEx rd( bvhdbl3( 0, 0, 0 ), bvhdbl3( 0, 0, 2 ) );
	OrderedHitQueryEx qd( bd, rd, 0.0 );
	std::vector<OrderedHitEx> got;
	OrderedHitEx h;
	while (qd.Next( h )) got.push_back( h );
	std::vector<RefHit<double, bvhdbl3>> ref;
	for (uint32_t p = 0; p < fs.count(); p++)
	{
		double t, u, v;
		if (refTriangle( rd.O, rd.D, d[p * 3], d[p * 3 + 1], d[p * 3 + 2], 0.0, rd.hit.t, t, u, v ))
			ref.push_back( { t, u, v, p, 0 } );
	}
	refSort( ref );
	c.check( got.size() == ref.size(), "orderDouble: count" );
	for (size_t i = 0; i < std::min( got.size(), ref.size() ); i++)
	{
		c.check( got[i].prim == ref[i].prim && got[i].inst == 0, "orderDouble: identity" );
		c.check( close( got[i].t, ref[i].t, DTOL ) && close( got[i].u, ref[i].u, DTOL ) && close( got[i].v, ref[i].v, DTOL ),
			"orderDouble: tight double tolerance" );
	}
}

void orderIndexedStride( ordered_test::Case& c )
{
	// Vertices in a padded (strided) layout, triangles addressed through an
	// index array that permutes both vertices and triangle order.
	const uint32_t N = 6;
	struct Padded { bvhvec4 v; float pad; };
	std::vector<Padded> buf( N * 3 );
	TriScene plain;
	auto addTri = [&]( float z ) {
		plain.verts.push_back( { -1, -1, z, 0 } ), plain.verts.push_back( { 1, -1, z, 0 } ), plain.verts.push_back( { 0, 1, z, 0 } );
	};
	for (uint32_t i = 0; i < N; i++) addTri( 1.0f + i );
	// Store vertices in reversed order; indices undo the reversal per triangle.
	for (uint32_t i = 0; i < N * 3; i++) buf[i].v = plain.verts[N * 3 - 1 - i];
	std::vector<uint32_t> vertIdx( N * 3 );
	for (uint32_t p = 0; p < N; p++) for (int k = 0; k < 3; k++)
		vertIdx[p * 3 + k] = N * 3 - 1 - ( p * 3 + k );
	bvhvec4slice slice( reinterpret_cast<const bvhvec4*>( buf.data() ), N * 3, sizeof( Padded ) );
	BVH bvh;
	bvh.Build( slice, vertIdx.data(), N );
	Ray ray( bvhvec3( 0, 0, 0 ), bvhvec3( 0, 0, 1 ) );
	OrderedHitQuery q( bvh, ray, 0.0f );
	const auto got = collect( q );
	// Reference walks the same strided, indexed layout.
	TriScene indexed;
	for (uint32_t p = 0; p < N; p++) for (int k = 0; k < 3; k++)
	{
		const bvhvec4& v = *reinterpret_cast<const bvhvec4*>(
			(const int8_t*)slice.data + (size_t)slice.stride * vertIdx[p * 3 + k] );
		indexed.verts.push_back( v );
	}
	const auto ref = referenceBLAS( indexed, ray, 0.0f );
	checkSequence( c, got, ref, "orderIndexedStride" );
	c.check( got.size() == N, "orderIndexedStride: all layers" );
}

// ----------------------------------------------------------------------------
// instances group
// ----------------------------------------------------------------------------

// Reference for a TLAS: enumerate every mask-passing instance and BLAS triangle.
std::vector<RefHit<float, bvhvec3>> referenceTLAS( const BVH& tlas, const Ray& ray, const float tmin,
	const std::vector<const TriScene*>& blasScenes )
{
	std::vector<RefHit<float, bvhvec3>> out;
	for (uint32_t i = 0; i < tlas.triCount; i++)
	{
		const BLASInstance& inst = tlas.instList[i];
		if (!(inst.mask & ray.mask)) continue;
		const bvhvec3 O = tinybvh_transform_point( ray.O, inst.invTransform );
		const bvhvec3 D = tinybvh_transform_vector( ray.D, inst.invTransform );
		const TriScene& sc = *blasScenes[inst.blasIdx];
		for (uint32_t p = 0; p < sc.count(); p++)
		{
			float t, u, v;
			if (refTriangle( O, D, sc.verts[p * 3], sc.verts[p * 3 + 1], sc.verts[p * 3 + 2],
				tmin, ray.hit.t, t, u, v ))
				out.push_back( { t, u, v, p, i } );
		}
	}
	refSort( out );
	return out;
}

void instancesInterleaved( ordered_test::Case& c )
{
	// blasFar: its AABB opens near the origin, but the triangle the ray hits is
	// far away; the second triangle only serves to stretch the box.
	TriScene farScene, nearScene;
	farScene.verts = {
		{ -10, 5, 0.5f, 0 }, { 10, 5, 0.5f, 0 }, { 0, 6, 9, 0 },  // miss, stretches the box
		{ -1, -1, 8, 0 }, { 1, -1, 8, 0 }, { 0, 1, 8, 0 }        // hit at t = 8
	};
	nearScene.verts = {
		{ -1, -1, 4, 0 }, { 1, -1, 4, 0 }, { 0, 1, 4, 0 }        // hit at t = 4
	};
	BVH blasFar, blasNear;
	blasFar.Build( farScene.verts.data(), farScene.count() );
	blasNear.Build( nearScene.verts.data(), nearScene.count() );
	BLASInstance inst[2] = { BLASInstance( 0 ), BLASInstance( 1 ) };
	BVHBase* blasses[2] = { &blasFar, &blasNear };
	BVH tlas;
	tlas.Build( inst, 2, blasses, 2 );

	Ray ray( bvhvec3( 0, 0, 0 ), bvhvec3( 0, 0, 1 ) );
	std::vector<const TriScene*> scenes = { &farScene, &nearScene };
	const auto ref = referenceTLAS( tlas, ray, 0.0f, scenes );
	OrderedHitQuery q( tlas, ray, 0.0f );
	OrderedHit first;
	c.check( q.Next( first ), "instancesInterleaved: first hit exists" );
	// The late-box / near-face instance must be delivered before the early-box
	// instance, whose BLAS has not even been entered at this point.
	c.check( first.inst == 1 && first.prim == 0 && close( first.t, 4.0f, FTOL ),
		"instancesInterleaved: near face wins over early box" );
	const auto st1 = q.GetStats();
	// The far box opens first, so its BLAS root may already be on the frontier;
	// but its distant triangle has not been tested: whole BLASes are never
	// drained ahead of nearer pending work.
	c.check( st1.triangleTests == 1, "instancesInterleaved: far triangle not tested yet" );
	c.check( st1.emittedHits == 1, "instancesInterleaved: one emission" );
	const auto got = [&] { auto v = collect( q ); v.insert( v.begin(), first ); return v; }();
	checkSequence( c, got, ref, "instancesInterleaved" );
	c.check( got[1].inst == 0 && close( got[1].t, 8.0f, FTOL ), "instancesInterleaved: far face second" );
}

void instancesSharedBlasMask( ordered_test::Case& c )
{
	TriScene sc;
	sc.verts = {
		{ -1, -1, 2, 0 }, { 1, -1, 2, 0 }, { 0, 1, 2, 0 },
		{ -1, -1, 4, 0 }, { 1, -1, 4, 0 }, { 0, 1, 4, 0 }
	};
	BVH blas;
	blas.Build( sc.verts.data(), sc.count() );
	BLASInstance inst[4] = { BLASInstance( 0 ), BLASInstance( 0 ), BLASInstance( 0 ), BLASInstance( 0 ) };
	inst[0].transform[0] = 2.0f; inst[0].transform[5] = 3.0f; inst[0].transform[11] = 1.0f; // non-uniform scale
	inst[1].transform[1] = 0.5f; inst[1].transform[11] = 0.5f;                              // shear
	inst[2].transform[0] = -1.0f; inst[2].transform[11] = 2.0f;                             // reflection
	inst[0].mask = 1; inst[1].mask = 2; inst[2].mask = 3; inst[3].mask = 0;
	BVHBase* blasses[1] = { &blas };
	BVH tlas;
	tlas.Build( inst, 4, blasses, 1 );
	std::vector<const TriScene*> scenes = { &sc };
	for (uint32_t rayMask : { 1u, 2u, 3u })
	{
		Ray ray( bvhvec3( 0, 0, 0 ), bvhvec3( 0, 0, 1 ), BVH_FAR, rayMask );
		const auto ref = referenceTLAS( tlas, ray, 0.0f, scenes );
		OrderedHitQuery q( tlas, ray, 0.0f );
		const auto got = collect( q );
		checkSequence( c, got, ref, "instancesSharedBlasMask" );
		// Every surviving instance contributes both triangles; mask-0 never appears.
		std::vector<uint32_t> seenInst;
		for (const auto& h : got) seenInst.push_back( h.inst );
		std::sort( seenInst.begin(), seenInst.end() );
		seenInst.erase( std::unique( seenInst.begin(), seenInst.end() ), seenInst.end() );
		std::vector<uint32_t> expect;
		for (uint32_t i = 0; i < 4; i++) if (inst[i].mask & rayMask) expect.push_back( i );
		c.check( seenInst == expect, "instancesSharedBlasMask: bitwise mask selection" );
		for (uint32_t i : expect)
		{
			const size_t cnt = std::count_if( got.begin(), got.end(), [i]( const OrderedHit& h ) { return h.inst == i; } );
			c.check( cnt == 2, "instancesSharedBlasMask: shared BLAS yields per-instance hits" );
		}
	}
}

void instancesDouble( ordered_test::Case& c )
{
	std::vector<bvhdbl3> d = {
		{ -1, -1, 2 }, { 1, -1, 2 }, { 0, 1, 2 },
		{ -1, -1, 4 }, { 1, -1, 4 }, { 0, 1, 4 }
	};
	BVH_Double blas;
	blas.Build( d.data(), 2 );
	BLASInstanceEx inst[3] = { BLASInstanceEx( 0 ), BLASInstanceEx( 0 ), BLASInstanceEx( 0 ) };
	inst[0].transform[11] = 1.0;
	inst[1].transform[0] = -1.0; inst[1].transform[11] = 1.0; // reflection, same depth
	inst[2].transform[11] = 5.0;
	inst[0].mask = 1; inst[1].mask = 1; inst[2].mask = 2;
	BVH_Double* blasses[1] = { &blas };
	BVH_Double tlas;
	tlas.Build( inst, 3, blasses, 1 );
	RayEx ray( bvhdbl3( 0, 0, 0 ), bvhdbl3( 0, 0, 1 ), BVH_DBL_FAR, 1 );
	OrderedHitQueryEx q( tlas, ray, 0.0 );
	std::vector<OrderedHitEx> got;
	OrderedHitEx h;
	while (q.Next( h )) got.push_back( h );
	// Independent double reference.
	std::vector<RefHit<double, bvhdbl3>> ref;
	for (uint32_t i = 0; i < 3; i++)
	{
		if (!(inst[i].mask & ray.mask)) continue;
		const bvhdbl3 O = tinybvh_transform_point( ray.O, inst[i].invTransform );
		const bvhdbl3 D = tinybvh_transform_vector( ray.D, inst[i].invTransform );
		for (uint32_t p = 0; p < 2; p++)
		{
			double t, u, v;
			if (refTriangle( O, D, d[p * 3], d[p * 3 + 1], d[p * 3 + 2], 0.0, ray.hit.t, t, u, v ))
				ref.push_back( { t, u, v, p, i } );
		}
	}
	refSort( ref );
	c.check( got.size() == ref.size(), "instancesDouble: count" );
	for (size_t i = 0; i < std::min( got.size(), ref.size() ); i++)
	{
		c.check( got[i].inst == ref[i].inst && got[i].prim == ref[i].prim, "instancesDouble: identity order" );
		c.check( close( got[i].t, ref[i].t, DTOL ), "instancesDouble: t tolerance" );
		c.check( close( got[i].u, ref[i].u, DTOL ) && close( got[i].v, ref[i].v, DTOL ), "instancesDouble: barycentrics" );
	}
	c.check( got.size() == 4 && got[0].inst == 0 && got[1].inst == 1, "instancesDouble: equal-depth instances both kept" );
}

void instancesMissAndEmpty( ordered_test::Case& c )
{
	TriScene sc;
	sc.verts = { { -1, -1, 2, 0 }, { 1, -1, 2, 0 }, { 0, 1, 2, 0 } };
	BVH blas;
	blas.Build( sc.verts.data(), 1 );
	BLASInstance inst[2] = { BLASInstance( 0 ), BLASInstance( 0 ) };
	inst[0].transform[11] = 4.0f; inst[0].mask = 1;
	inst[1].transform[11] = 8.0f; inst[1].mask = 2;
	BVHBase* blasses[1] = { &blas };
	BVH tlas;
	tlas.Build( inst, 2, blasses, 1 );
	// Ray aimed away from every instance: empty result, no entries.
	Ray miss( bvhvec3( 0, 0, 0 ), bvhvec3( 5, 0, 1 ) );
	OrderedHitQuery q( tlas, miss, 0.0f );
	c.check( collect( q ).empty(), "instancesMiss: no hits" );
	c.check( q.GetStats().instanceEntries == 0, "instancesMiss: no instance entered" );
	// Mask selects nothing.
	Ray masked( bvhvec3( 0, 0, 0 ), bvhvec3( 0, 0, 1 ), BVH_FAR, 4 );
	OrderedHitQuery q2( tlas, masked, 0.0f );
	c.check( collect( q2 ).empty(), "instancesMiss: mask excludes all" );
	c.check( q2.GetStats().instanceEntries == 0, "instancesMiss: masked instances never entered" );
	// A finite tmax ending before the first instance is empty as well.
	Ray shortRay( bvhvec3( 0, 0, 0 ), bvhvec3( 0, 0, 1 ), 3.0f );
	OrderedHitQuery q3( tlas, shortRay, 0.0f );
	c.check( collect( q3 ).empty(), "instancesMiss: tmax below nearest instance" );
}

// ----------------------------------------------------------------------------
// frontier group
// ----------------------------------------------------------------------------

TriScene makeLayeredScene( const uint32_t n )
{
	TriScene s;
	for (uint32_t i = 0; i < n; i++)
	{
		const float z = 1.0f + i * 0.5f;
		s.verts.push_back( { -0.5f, -0.5f, z, 0 } );
		s.verts.push_back( { 0.5f, -0.5f, z, 0 } );
		s.verts.push_back( { 0.0f, 0.5f, z, 0 } );
	}
	return s;
}

void frontierBlasEarlyStop( ordered_test::Case& c )
{
	const TriScene sc = makeLayeredScene( 4096 );
	BVH bvh;
	bvh.Build( sc.verts.data(), sc.count() );
	Ray ray( bvhvec3( 0, 0, 0 ), bvhvec3( 0, 0, 1 ) );
	OrderedHitQuery q( bvh, ray, 0.0f );
	OrderedHit h;
	for (int k = 0; k < 2; k++) c.check( q.Next( h ), "frontierBlas: two hits available" );
	const auto early = q.GetStats();
	c.check( early.triangleTests <= 64, "frontierBlas: triangleTests <= 64 after two hits" );
	c.check( early.triangleTests == 2, "frontierBlas: exactly the two delivered tris tested" );
	c.check( early.emittedHits == 2, "frontierBlas: emitted counter" );
	c.check( h.prim == 1 && close( h.t, 1.5f, FTOL ), "frontierBlas: second hit identity" );
	// Full consumption then matches the exhaustive reference, with monotone counters.
	const auto got = collect( q );
	const auto st = q.GetStats();
	c.check( got.size() == 4094 && st.emittedHits == 4096, "frontierBlas: full identity count" );
	c.check( st.triangleTests == 4096, "frontierBlas: each triangle tested once" );
	c.check( st.nodeVisits >= early.nodeVisits && st.triangleTests >= early.triangleTests,
		"frontierBlas: counters are cumulative" );
	for (uint32_t i = 0; i < 4094; i++)
	{
		c.check( got[i].prim == i + 2, "frontierBlas: prim order" );
		c.check( i == 0 || got[i].t >= got[i - 1].t, "frontierBlas: monotone t" );
	}
	OrderedHit untouched{ 9, 8, 7, 6, 5 };
	const OrderedHit copy = untouched;
	c.check( !q.Next( untouched ), "frontierBlas: terminates" );
	c.check( untouched.prim == copy.prim && untouched.inst == copy.inst, "frontierBlas: out preserved" );
}

void frontierTlasEarlyStop( ordered_test::Case& c )
{
	const TriScene one = makeLayeredScene( 1 );
	BVH blas;
	blas.Build( one.verts.data(), 1 );
	const uint32_t N = 4096;
	std::vector<BLASInstance> inst( N );
	std::vector<BVHBase*> blasses( N );
	for (uint32_t i = 0; i < N; i++)
	{
		inst[i] = BLASInstance( 0 );
		inst[i].transform[11] = 1.0f + i * 0.5f;
		blasses[i] = &blas;
	}
	BVH tlas;
	tlas.Build( inst.data(), N, blasses.data(), N );
	Ray ray( bvhvec3( 0, 0, 0 ), bvhvec3( 0, 0, 1 ) );
	OrderedHitQuery q( tlas, ray, 0.0f );
	OrderedHit h;
	for (int k = 0; k < 2; k++) c.check( q.Next( h ), "frontierTlas: two hits available" );
	const auto early = q.GetStats();
	c.check( early.instanceEntries <= 64, "frontierTlas: instanceEntries <= 64 after two hits" );
	c.check( early.instanceEntries == 2 && early.triangleTests == 2, "frontierTlas: only reached instances do work" );
	const auto got = collect( q );
	c.check( got.size() == N - 2, "frontierTlas: remaining hits" );
	c.check( q.GetStats().instanceEntries == N && q.GetStats().emittedHits == N, "frontierTlas: all instances once" );
	for (uint32_t i = 0; i < N - 2; i++)
	{
		c.check( got[i].inst == i + 2 && got[i].prim == 0, "frontierTlas: instance order and original prim" );
		c.check( i == 0 || got[i].t >= got[i - 1].t, "frontierTlas: monotone t" );
	}
}

// SBVH material that reliably produces spatial splits in both scalar and
// native builds: a dense triangle cluster pierced by long spike triangles.
TriScene makeSplitScene( const int grid, const int spikes )
{
	TriScene s;
	for (int i = 0; i < grid * grid; i++)
	{
		const float x = -2.0f + 4.0f * ( i % grid ) / ( grid - 1 );
		const float y = -2.0f + 4.0f * ( i / grid ) / ( grid - 1 );
		s.verts.push_back( { x - 0.15f, y - 0.15f, -0.5f, 0 } );
		s.verts.push_back( { x + 0.15f, y - 0.15f, -0.5f, 0 } );
		s.verts.push_back( { x, y + 0.15f, 0.5f, 0 } );
	}
	for (int k = 0; k < spikes; k++)
	{
		const float a = 6.2831853f * k / spikes;
		s.verts.push_back( { 0, 0, 0, 0 } );
		s.verts.push_back( { 3 * cosf( a ), 3 * sinf( a ), -1, 0 } );
		s.verts.push_back( { 3 * cosf( a ), 3 * sinf( a ), 1, 0 } );
	}
	return s;
}

void frontierSpatialSplits( ordered_test::Case& c )
{
	const TriScene sc = makeSplitScene( 60, 32 );
	BVH bvh;
	bvh.settings.binnedBVH = true; // binned SBVH; spatial splits in both build modes
	bvh.BuildHQ( sc.verts.data(), sc.count() );
	c.check( bvh.idxCount > bvh.triCount, "frontierSplits: split capacity consumed" );
	// Count leaf references per original primitive, leaf by leaf.
	std::vector<std::vector<uint32_t>> leavesOf( sc.count() );
	uint32_t duplicated = 0;
	for (uint32_t n = 0; n < bvh.usedNodes; n++)
	{
		const auto& node = bvh.bvhNode[n];
		if (!node.isLeaf()) continue;
		for (uint32_t i = 0; i < node.triCount; i++)
			leavesOf[bvh.primIdx[node.leftFirst + i]].push_back( n );
	}
	for (uint32_t p = 0; p < sc.count(); p++)
	{
		std::sort( leavesOf[p].begin(), leavesOf[p].end() );
		leavesOf[p].erase( std::unique( leavesOf[p].begin(), leavesOf[p].end() ), leavesOf[p].end() );
		if (leavesOf[p].size() >= 2) duplicated++;
	}
	c.check( duplicated >= 10, "frontierSplits: primitives actually present in two+ distinct leaves" );
	// Rays through the duplicated region with a slight offset so triangle edges
	// are not hit exactly: FP-rounding edge cases are covered by orderAxisRays.
	const Ray ray( bvhvec3( 0.13f, -0.27f, -3 ), bvhvec3( 0.07f, 0.11f, 1 ) );
	const auto ref = referenceBLAS( sc, ray, 0.0f );
	OrderedHitQuery q( bvh, ray, 0.0f );
	const auto got = collect( q );
	checkSequence( c, got, ref, "frontierSplits" );
	std::vector<uint32_t> prims;
	for (const auto& h : got) prims.push_back( h.prim );
	std::sort( prims.begin(), prims.end() );
	c.check( std::unique( prims.begin(), prims.end() ) == prims.end(), "frontierSplits: no duplicate delivery" );
	c.check( q.GetStats().triangleTests <= bvh.triCount, "frontierSplits: one test per primitive" );
	// A duplicated primitive that the ray hits is reported exactly once.
	const uint32_t dupPrim = (uint32_t)( std::find_if( leavesOf.begin(), leavesOf.end(),
		[]( const std::vector<uint32_t>& l ) { return l.size() >= 2; } ) - leavesOf.begin() );
	const size_t dupHits = std::count_if( got.begin(), got.end(), [dupPrim]( const OrderedHit& h ) { return h.prim == dupPrim; } );
	c.check( dupHits <= 1, "frontierSplits: known duplicated primitive delivered at most once" );

	// Two instances sharing the split BLAS: per-instance hits must both survive.
	BLASInstance inst[2] = { BLASInstance( 0 ), BLASInstance( 0 ) };
	inst[0].transform[11] = 0.0f;
	inst[1].transform[11] = 6.0f;
	BVHBase* blasses[1] = { &bvh };
	BVH tlas;
	tlas.Build( inst, 2, blasses, 1 );
	Ray r2( bvhvec3( 0.13f, -0.27f, -3 ), bvhvec3( 0.07f, 0.11f, 1 ) );
	OrderedHitQuery q2( tlas, r2, 0.0f );
	const auto tgot = collect( q2 );
	std::vector<const TriScene*> scenes = { &sc };
	const auto tref = referenceTLAS( tlas, r2, 0.0f, scenes );
	checkSequence( c, tgot, tref, "frontierSplits tlas" );
	c.check( tgot.size() == tref.size(), "frontierSplits: no instance hits lost" );
	for (uint32_t which = 0; which < 2; which++)
	{
		const size_t gotCnt = std::count_if( tgot.begin(), tgot.end(), [which]( const OrderedHit& h ) { return h.inst == which; } );
		const size_t refCnt = std::count_if( tref.begin(), tref.end(), [which]( const auto& h ) { return h.inst == which; } );
		c.check( gotCnt >= 1, "frontierSplits: each instance keeps its own hits" );
		c.check( gotCnt == refCnt, "frontierSplits: per-instance count matches reference" );
	}
}

void frontierInterleave( ordered_test::Case& c )
{
	const TriScene sc = makeLayeredScene( 128 );
	BVH bvh;
	bvh.Build( sc.verts.data(), sc.count() );
	Ray r1( bvhvec3( 0, 0, 0 ), bvhvec3( 0, 0, 1 ) );
	Ray r2( bvhvec3( 0.2f, 0.1f, -1 ), bvhvec3( 0, 0, 1 ) );
	OrderedHitQuery q1( bvh, r1, 0.0f ), q2( bvh, r2, 0.0f );
	std::vector<OrderedHit> a, b;
	OrderedHit h1, h2;
	// Strict alternation: the queries must not share frontier state.
	for (int round = 0; round < 40; round++)
	{
		if (q1.Next( h1 )) a.push_back( h1 );
		if (q2.Next( h2 )) b.push_back( h2 );
	}
	while (q1.Next( h1 )) a.push_back( h1 );
	while (q2.Next( h2 )) b.push_back( h2 );
	checkSequence( c, a, referenceBLAS( sc, r1, 0.0f ), "frontierInterleave a" );
	checkSequence( c, b, referenceBLAS( sc, r2, 0.0f ), "frontierInterleave b" );
}

void frontierThreads( ordered_test::Case& c )
{
	const TriScene sc = makeSplitScene( 30, 16 );
	BVH bvh;
	bvh.settings.binnedBVH = true;
	bvh.BuildHQ( sc.verts.data(), sc.count() );
	const Ray rays[2] = {
		Ray( bvhvec3( 0.13f, -0.27f, -3 ), bvhvec3( 0.07f, 0.11f, 1 ) ),
		Ray( bvhvec3( 0.3f, -0.2f, -3 ), bvhvec3( -0.1f, 0.05f, 1 ) )
	};
	std::vector<OrderedHit> got[2];
	std::thread workers[2];
	for (int t = 0; t < 2; t++) workers[t] = std::thread( [&, t]
	{
		OrderedHitQuery q( bvh, rays[t], 0.0f ); // same read-only scene, distinct queries
		got[t] = collect( q );
	} );
	for (auto& w : workers) w.join();
	for (int t = 0; t < 2; t++) checkSequence( c, got[t], referenceBLAS( sc, rays[t], 0.0f ), "frontierThreads" );
}

// ----------------------------------------------------------------------------
// render group
// ----------------------------------------------------------------------------

std::vector<int> readPPM( const char* path, ordered_test::Case& c )
{
	std::ifstream f( path );
	c.check( f.good(), std::string( "render: missing image " ) + path );
	std::string token;
	f >> token;
	c.check( token == "P3", "render: P3 magic" );
	int w, h, mx;
	f >> w >> h >> mx;
	c.check( w == ordered_layers_demo::IMG_SIZE && h == ordered_layers_demo::IMG_SIZE && mx == 255, "render: 32x32x255" );
	std::vector<int> vals;
	int v;
	while (f >> v) vals.push_back( v );
	c.check( vals.size() == (size_t)3 * w * h, "render: pixel count" );
	return vals;
}

// Independent brute-force renderer for the shared scene: no ordered query and
// no BVH intersection is used.
struct RGB { float r, g, b; };
RGB referencePixel( const int px, const int py )
{
	using namespace ordered_layers_demo;
	const auto layers = BuildScene();
	const float dx = ( ( px + 0.5f ) / IMG_SIZE * 2.0f - 1.0f ) * 0.9f;
	const float dy = ( ( py + 0.5f ) / IMG_SIZE * 2.0f - 1.0f ) * 0.9f;
	const bvhvec3 O( 0, 0, 0 ), D( dx, dy, 1.0f );
	struct RH { float t; uint32_t prim; };
	std::vector<RH> hits;
	for (uint32_t p = 0; p < layers.size(); p++)
	{
		const bvhvec4 a( layers[p].v[0][0], layers[p].v[0][1], layers[p].v[0][2], 0 );
		const bvhvec4 b( layers[p].v[1][0], layers[p].v[1][1], layers[p].v[1][2], 0 );
		const bvhvec4 cc( layers[p].v[2][0], layers[p].v[2][1], layers[p].v[2][2], 0 );
		float t, u, v;
		if (refTriangle( O, D, a, b, cc, 0.0f, BVH_FAR, t, u, v )) hits.push_back( { t, p } );
	}
	std::sort( hits.begin(), hits.end(), []( const RH& a, const RH& b )
	{
		if (a.t != b.t) return a.t < b.t;
		return a.prim < b.prim;
	} );
	RGB pixel{ BG_R, BG_G, BG_B };
	float trans = 1.0f;
	for (const RH& h : hits)
	{
		if (trans < CUTOFF) break;
		const Layer& L = layers[h.prim];
		const float w = trans * L.alpha;
		pixel.r += w * L.r * 255.0f;
		pixel.g += w * L.g * 255.0f;
		pixel.b += w * L.b * 255.0f;
		trans *= ( 1.0f - L.alpha );
	}
	return pixel;
}

void renderPixels( ordered_test::Case& c )
{
	using namespace ordered_layers_demo;
	const std::vector<int> vals = readPPM( ".cache/ordered/ordered_layers.ppm", c );
	const std::tuple<int, int, int> bg = { (int)BG_R, (int)BG_G, (int)BG_B };
	int bgCount = 0;
	for (int py = 0; py < IMG_SIZE; py++) for (int px = 0; px < IMG_SIZE; px++)
	{
		const int i = ( py * IMG_SIZE + px ) * 3;
		const std::tuple<int, int, int> got = { vals[i], vals[i + 1], vals[i + 2] };
		const RGB ref = referencePixel( px, py );
		auto near8 = []( int a, float b ) { return std::fabs( a - b ) <= 2.0f; };
		c.check( near8( std::get<0>( got ), ref.r ) && near8( std::get<1>( got ), ref.g ) && near8( std::get<2>( got ), ref.b ),
			"renderPixels: pixel matches brute-force composite" );
		if (got == bg) bgCount++;
	}
	// Both background and actual surfaces must be visible.
	c.check( bgCount > 0 && bgCount < IMG_SIZE * IMG_SIZE, "renderPixels: background and surfaces both visible" );
	const auto corner = [&]( int x, int y ) { const int i = ( y * IMG_SIZE + x ) * 3; return std::make_tuple( vals[i], vals[i + 1], vals[i + 2] ); };
	c.check( corner( 0, 0 ) == bg && corner( 31, 31 ) == bg, "renderPixels: corners keep background" );
}

void renderEarlyStop( ordered_test::Case& c )
{
	using namespace ordered_layers_demo;
	const auto layers = BuildScene();
	std::vector<bvhvec4> verts;
	for (const Layer& L : layers) for (int k = 0; k < 3; k++)
		verts.push_back( bvhvec4( L.v[k][0], L.v[k][1], L.v[k][2], 0 ) );
	BVH bvh;
	bvh.Build( verts.data(), (uint32_t)layers.size() );

	// Dense-center pixel: stop as soon as transmittance crosses 1%, then prove
	// the paused query still holds further hits.
	const int px = 14, py = 14;
	const float dx = ( ( px + 0.5f ) / IMG_SIZE * 2.0f - 1.0f ) * 0.9f;
	const float dy = ( ( py + 0.5f ) / IMG_SIZE * 2.0f - 1.0f ) * 0.9f;
	Ray ray( bvhvec3( 0, 0, 0 ), bvhvec3( dx, dy, 1.0f ) );
	OrderedHitQuery q( bvh, ray, 0.0f );
	const RGB ref = referencePixel( px, py );
	c.check( ref.r != BG_R || ref.g != BG_G || ref.b != BG_B, "renderEarlyStop: center ray hits geometry" );
	OrderedHit h;
	std::vector<OrderedHit> consumed;
	float trans = 1.0f;
	while (trans >= CUTOFF && q.Next( h ))
	{
		consumed.push_back( h );
		trans *= ( 1.0f - layers[h.prim].alpha );
	}
	c.check( trans < CUTOFF, "renderEarlyStop: cutoff reached" );
	c.check( !consumed.empty(), "renderEarlyStop: layers were composited" );
	OrderedHit more;
	c.check( q.Next( more ), "renderEarlyStop: paused query still has unconsumed hits" );
	// The first two delivered hits match the brute-force front two exactly in
	// identity, and a query abandoned after two hits does much less work.
	std::vector<OrderedHit> front2;
	OrderedHitQuery q2( bvh, ray, 0.0f );
	OrderedHit x;
	while (front2.size() < 2 && q2.Next( x )) front2.push_back( x );
	auto refHits = [&]
	{
		struct RH { float t; uint32_t prim; };
		std::vector<RH> hits;
		for (uint32_t p = 0; p < layers.size(); p++)
		{
			float t, u, v;
			const bvhvec4 a( layers[p].v[0][0], layers[p].v[0][1], layers[p].v[0][2], 0 );
			const bvhvec4 b( layers[p].v[1][0], layers[p].v[1][1], layers[p].v[1][2], 0 );
			const bvhvec4 cc( layers[p].v[2][0], layers[p].v[2][1], layers[p].v[2][2], 0 );
			if (refTriangle( ray.O, ray.D, a, b, cc, 0.0f, BVH_FAR, t, u, v )) hits.push_back( { t, p } );
		}
		std::sort( hits.begin(), hits.end(), []( const RH& a, const RH& b ) { return a.t != b.t ? a.t < b.t : a.prim < b.prim; } );
		return hits;
	}();
	c.check( front2.size() >= 2 && refHits.size() > 2, "renderEarlyStop: enough layers for the check" );
	c.check( front2[0].prim == refHits[0].prim && front2[1].prim == refHits[1].prim,
		"renderEarlyStop: first two hits match reference order" );
	c.check( close( front2[0].t, refHits[0].t, FTOL ) && close( front2[1].t, refHits[1].t, FTOL ),
		"renderEarlyStop: first two hit distances" );
	c.check( q2.GetStats().triangleTests < q.GetStats().triangleTests || q.GetStats().emittedHits > 2,
		"renderEarlyStop: early abandonment avoids full traversal" );
}

} // namespace

void register_ordered_tests( ordered_test::Registry& reg )
{
	reg.add( "order", "blas_full_sequence", orderBlasFull );
	reg.add( "order", "exact_tie_order", orderExactTies );
	reg.add( "order", "half_open_interval", orderInterval );
	reg.add( "order", "axis_and_signed_zero", orderAxisRays );
	reg.add( "order", "empty_query_and_double", orderEmptyAndDouble );
	reg.add( "order", "indexed_stride_layout", orderIndexedStride );

	reg.add( "instances", "interleaved_box_face", instancesInterleaved );
	reg.add( "instances", "shared_blas_affine_mask", instancesSharedBlasMask );
	reg.add( "instances", "double_tlas", instancesDouble );
	reg.add( "instances", "miss_mask_and_range", instancesMissAndEmpty );

	reg.add( "frontier", "blas_early_stop_counts", frontierBlasEarlyStop );
	reg.add( "frontier", "tlas_early_stop_counts", frontierTlasEarlyStop );
	reg.add( "frontier", "spatial_split_dedup", frontierSpatialSplits );
	reg.add( "frontier", "interleaved_queries", frontierInterleave );
	reg.add( "frontier", "concurrent_read_only", frontierThreads );

	reg.add( "render", "ppm_pixel_reference", renderPixels );
	reg.add( "render", "cutoff_pauses_query", renderEarlyStop );
}
