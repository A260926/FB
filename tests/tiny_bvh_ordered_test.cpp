// Tests for the pausable ordered hit query in tiny_bvh_ordered.h.
//
// The reference is an independent world-space brute force: every triangle is
// enumerated (contiguous vertices, or a strided slice plus an index array) and
// intersected with a standalone Moeller-Trumbore implementation. It never uses
// OrderedHitQuery or BVH::Intersect. The fixed driver supplies main(); this
// translation unit only registers its cases.

#define TINYBVH_IMPLEMENTATION
#include "tiny_bvh.h"
#include "tiny_bvh_ordered.h"
#include "task_support/registry.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

using namespace tinybvh;

namespace {

constexpr float F_EPS = 2e-5f;
constexpr double D_EPS = 1e-10;

bool CloseF( const float a, const float b ) { return std::fabs( a - b ) <= F_EPS + F_EPS * std::fabs( b ); }
bool CloseD( const double a, const double b ) { return std::fabs( a - b ) <= D_EPS + D_EPS * std::fabs( b ); }

// ----------------------------------------------------------------------------
// Independent reference intersection (world space, double precision math).
// ----------------------------------------------------------------------------

struct RefHit
{
	double t, u, v;
	uint64_t inst, prim;
};

bool RefTri( const bvhvec3& O, const bvhvec3& D, const float tmin, const float tmax,
	const bvhvec3& v0, const bvhvec3& v1, const bvhvec3& v2, double& t, double& u, double& v )
{
	const double d0[3] = { v0.x, v0.y, v0.z }, d1[3] = { v1.x, v1.y, v1.z }, d2[3] = { v2.x, v2.y, v2.z };
	const double e1[3] = { d1[0] - d0[0], d1[1] - d0[1], d1[2] - d0[2] };
	const double e2[3] = { d2[0] - d0[0], d2[1] - d0[1], d2[2] - d0[2] };
	const double rd[3] = { D.x, D.y, D.z }, ro[3] = { O.x, O.y, O.z };
	const double hx = rd[1] * e2[2] - rd[2] * e2[1];
	const double hy = rd[2] * e2[0] - rd[0] * e2[2];
	const double hz = rd[0] * e2[1] - rd[1] * e2[0];
	const double a = e1[0] * hx + e1[1] * hy + e1[2] * hz;
	if (a == 0.0) return false; // zero-area triangle
	const double f = 1.0 / a;
	const double sx = ro[0] - d0[0], sy = ro[1] - d0[1], sz = ro[2] - d0[2];
	u = f * (sx * hx + sy * hy + sz * hz);
	if (!(u >= 0.0 && u <= 1.0)) return false;
	const double qx = sy * e1[2] - sz * e1[1];
	const double qy = sz * e1[0] - sx * e1[2];
	const double qz = sx * e1[1] - sy * e1[0];
	v = f * (rd[0] * qx + rd[1] * qy + rd[2] * qz);
	if (!(v >= 0.0 && u + v <= 1.0)) return false;
	t = f * (e2[0] * qx + e2[1] * qy + e2[2] * qz);
	return t >= (double)tmin && t < (double)tmax;
}

bool RefTriD( const bvhdbl3& O, const bvhdbl3& D, const double tmin, const double tmax,
	const bvhdbl3& v0, const bvhdbl3& v1, const bvhdbl3& v2, double& t, double& u, double& v )
{
	const double e1[3] = { v1.x - v0.x, v1.y - v0.y, v1.z - v0.z };
	const double e2[3] = { v2.x - v0.x, v2.y - v0.y, v2.z - v0.z };
	const double hx = D.y * e2[2] - D.z * e2[1];
	const double hy = D.z * e2[0] - D.x * e2[2];
	const double hz = D.x * e2[1] - D.y * e2[0];
	const double a = e1[0] * hx + e1[1] * hy + e1[2] * hz;
	if (a == 0.0) return false;
	const double f = 1.0 / a;
	const double sx = O.x - v0.x, sy = O.y - v0.y, sz = O.z - v0.z;
	u = f * (sx * hx + sy * hy + sz * hz);
	if (!(u >= 0.0 && u <= 1.0)) return false;
	const double qx = sy * e1[2] - sz * e1[1];
	const double qy = sz * e1[0] - sx * e1[2];
	const double qz = sx * e1[1] - sy * e1[0];
	v = f * (D.x * qx + D.y * qy + D.z * qz);
	if (!(v >= 0.0 && u + v <= 1.0)) return false;
	t = f * (e2[0] * qx + e2[1] * qy + e2[2] * qz);
	return t >= tmin && t < tmax;
}

bool RefLess( const RefHit& a, const RefHit& b )
{
	if (a.t != b.t) return a.t < b.t;
	if (a.inst != b.inst) return a.inst < b.inst;
	return a.prim < b.prim;
}

// Enumerate a BLAS geometry in world space (float): supports contiguous
// vertices and index arrays; instance transform defaults to identity.
void RefEnumBLAS( const BVH& blas, const bvhmat4& world, std::vector<RefHit>& out,
	const Ray& ray, const float tmin, const float tmax, const uint64_t instId )
{
	const bool indexed = blas.vertIdx != 0;
	for (uint32_t prim = 0; prim < blas.triCount; prim++)
	{
		uint32_t i0, i1, i2;
		if (indexed) { i0 = blas.vertIdx[prim * 3]; i1 = blas.vertIdx[prim * 3 + 1]; i2 = blas.vertIdx[prim * 3 + 2]; }
		else { i0 = prim * 3; i1 = prim * 3 + 1; i2 = prim * 3 + 2; }
		bvhvec3 p[3] = { bvhvec3( blas.verts[i0] ), bvhvec3( blas.verts[i1] ), bvhvec3( blas.verts[i2] ) };
		if (instId != 0u || world[0] != 1.0f || world[5] != 1.0f || world[10] != 1.0f)
			for (auto& q : p) q = tinybvh_transform_point( q, world );
		double t, u, v;
		if (RefTri( ray.O, ray.D, tmin, tmax, p[0], p[1], p[2], t, u, v ))
			out.push_back( RefHit{ t, u, v, instId, prim } );
	}
}

std::vector<RefHit> RefBLAS( const BVH& blas, const Ray& ray, const float tmin, const float tmax )
{
	std::vector<RefHit> hits;
	bvhmat4 identity;
	RefEnumBLAS( blas, identity, hits, ray, tmin, tmax, 0 );
	std::sort( hits.begin(), hits.end(), RefLess );
	return hits;
}

std::vector<RefHit> RefTLAS( const BVH& tlas, const Ray& ray, const float tmin, const float tmax )
{
	std::vector<RefHit> hits;
	for (uint32_t instId = 0; instId < tlas.triCount; instId++)
	{
		const BLASInstance& inst = tlas.instList[instId];
		if (!(inst.mask & ray.mask)) continue;
		const BVH* blas = (const BVH*)tlas.blasList[inst.blasIdx];
		RefEnumBLAS( *blas, inst.transform, hits, ray, tmin, tmax, instId );
	}
	std::sort( hits.begin(), hits.end(), RefLess );
	return hits;
}

// Double-precision TLAS brute force: every instance triangle is transformed to
// world space and intersected with the standalone double test.
std::vector<RefHit> RefTLASEx( const BVH_Double& tlas, const RayEx& ray, const double tmin, const double tmax )
{
	std::vector<RefHit> hits;
	for (uint64_t instId = 0; instId < tlas.triCount; instId++)
	{
		const BLASInstanceEx& inst = tlas.instList[instId];
		if (!(inst.mask & ray.mask)) continue;
		const BVH_Double* blas = (const BVH_Double*)tlas.blasList[inst.blasIdx];
		for (uint64_t prim = 0; prim < blas->triCount; prim++)
		{
			bvhdbl3 p[3] = { blas->verts[prim * 3], blas->verts[prim * 3 + 1], blas->verts[prim * 3 + 2] };
			for (auto& q : p) q = tinybvh_transform_point( q, inst.transform );
			double t, u, v;
			if (RefTriD( ray.O, ray.D, tmin, tmax, p[0], p[1], p[2], t, u, v ))
				hits.push_back( RefHit{ t, u, v, (uint64_t)instId, prim } );
		}
	}
	std::sort( hits.begin(), hits.end(), RefLess );
	return hits;
}

// Full query consumption, checked against a sorted reference hit list.
void CheckAgainstRef( ordered_test::Case& c, const BVH& bvh, const Ray& ray,
	const float tmin, const std::vector<RefHit>& ref, const bool isTLAS )
{
	OrderedHitQuery q( bvh, ray, tmin );
	std::vector<OrderedHit> got;
	OrderedHit h;
	while (q.Next( h )) got.push_back( h );
	c.check( got.size() == ref.size(), "hit count matches brute force" );
	OrderedHit prev{};
	for (size_t i = 0; i < got.size(); i++)
	{
		if (i > 0)
		{
			const bool ordered = (got[i - 1].t < got[i].t) ||
				(got[i - 1].t == got[i].t && (got[i - 1].inst < got[i].inst ||
					(got[i - 1].inst == got[i].inst && got[i - 1].prim <= got[i].prim)));
			c.check( ordered, "query output is (t,inst,prim) sorted" );
		}
		prev = got[i];
		const RefHit& r = ref[i];
		c.check( got[i].prim == (uint32_t)r.prim, "primitive identity" );
		c.check( got[i].inst == (uint32_t)r.inst, "instance identity" );
		c.check( isTLAS || got[i].inst == 0u, "BLAS hits carry inst 0" );
		c.check( CloseF( got[i].t, (float)r.t ), "t within float tolerance" );
		c.check( CloseF( got[i].u, (float)r.u ), "u within float tolerance" );
		c.check( CloseF( got[i].v, (float)r.v ), "v within float tolerance" );
		c.check( got[i].t >= tmin && got[i].t < ray.hit.t, "hit inside [tmin, ray.hit.t)" );
	}
}

// ----------------------------------------------------------------------------
// Scene builders.
// ----------------------------------------------------------------------------

// N large horizontal triangles at z = 1..N, all covering the central ray.
std::vector<bvhvec4> LayeredTris( const uint32_t n )
{
	std::vector<bvhvec4> v( (size_t)n * 3 );
	for (uint32_t i = 0; i < n; i++)
	{
		const float z = (float)(i + 1);
		v[i * 3 + 0] = bvhvec4( -10, -10, z, 0 );
		v[i * 3 + 1] = bvhvec4( 10, -10, z, 0 );
		v[i * 3 + 2] = bvhvec4( 0, 10, z, 0 );
	}
	return v;
}

// Padded vertex: forces a slice stride larger than one vertex.
struct PaddedVertex { bvhvec4 p; float pad[3]; };

} // namespace

// ============================================================================
// order group
// ============================================================================

static void OrderBlasReference( ordered_test::Case& c )
{
	const uint32_t N = 257;
	const auto verts = LayeredTris( N );
	BVH bvh;
	bvh.Build( verts.data(), N );
	// A tilted ray, non-unit direction, starting between two layers.
	const Ray ray( bvhvec3( 0.1f, 0.2f, -2.0f ), bvhvec3( 0.05f, -0.02f, 1.5f ), 1000.0f );
	const auto ref = RefBLAS( bvh, ray, 0.0f, ray.hit.t );
	c.check( ref.size() > 100, "reference scene is genuinely layered" );
	CheckAgainstRef( c, bvh, ray, 0.0f, ref, false );

	// Finite interval: only a sub-range of the layers is delivered.
	const Ray ray2( ray.O, ray.D, 40.0f );
	const auto ref2 = RefBLAS( bvh, ray2, 7.0f, 40.0f );
	c.check( !ref2.empty() && ref2.size() < ref.size(), "finite interval trims the hit list" );
	CheckAgainstRef( c, bvh, ray2, 7.0f, ref2, false );

	// tmin exactly equal to a hit: the hit is still included (closed lower bound).
	const Ray axis( bvhvec3( 0.1f, 0.2f, 0.0f ), bvhvec3( 0.0f, 0.0f, 1.0f ), 100.0f );
	const auto ref3 = RefBLAS( bvh, axis, 5.0f, 100.0f );
	c.check( !ref3.empty() && ref3.front().t == 5.0, "tmin coincides with a hit" );
	CheckAgainstRef( c, bvh, axis, 5.0f, ref3, false );

	// The mid-point BuildQuick tree must deliver the identical ordered set.
	BVH quick;
	quick.BuildQuick( verts.data(), N );
	CheckAgainstRef( c, quick, ray, 0.0f, ref, false );

	// A spatially-split BuildHQ BLAS queried directly (not only under a TLAS).
	BVH hq;
	hq.settings.binnedBVH = true;
	hq.BuildHQ( verts.data(), N );
	CheckAgainstRef( c, hq, ray, 0.0f, ref, false );
}

static void OrderIndexedAndStride( ordered_test::Case& c )
{
	// Build an indexed, strided mesh: shuffled index buffer, 32-byte stride.
	const uint32_t N = 64;
	const auto flat = LayeredTris( N );
	std::vector<PaddedVertex> padded( (size_t)N * 3 );
	for (size_t i = 0; i < padded.size(); i++) padded[i].p = flat[i];
	std::vector<uint32_t> indices( (size_t)N * 3 );
	for (uint32_t t = 0; t < N; t++)
	{
		const uint32_t src = N - 1 - t; // reverse triangle order
		indices[t * 3 + 0] = src * 3 + 0;
		indices[t * 3 + 1] = src * 3 + 1;
		indices[t * 3 + 2] = src * 3 + 2;
	}
	bvhvec4slice slice( &padded[0].p, N * 3, sizeof( PaddedVertex ) );
	BVH bvh;
	bvh.Build( slice, indices.data(), N );
	c.check( bvh.isIndexed(), "builder kept the index array" );

	const Ray ray( bvhvec3( 0.1f, 0.2f, 0.0f ), bvhvec3( 0.0f, 0.0f, 1.0f ), 100.0f );
	// Reference reads the source arrays directly, with its own indexing.
	std::vector<RefHit> ref;
	for (uint32_t prim = 0; prim < N; prim++)
	{
		const bvhvec3 p0 = padded[indices[prim * 3]].p;
		const bvhvec3 p1 = padded[indices[prim * 3 + 1]].p;
		const bvhvec3 p2 = padded[indices[prim * 3 + 2]].p;
		double t, u, v;
		if (RefTri( ray.O, ray.D, 0.0f, ray.hit.t, p0, p1, p2, t, u, v ))
			ref.push_back( RefHit{ t, u, v, 0, prim } );
	}
	std::sort( ref.begin(), ref.end(), RefLess );
	CheckAgainstRef( c, bvh, ray, 0.0f, ref, false );
}

static void OrderEqualDistance( ordered_test::Case& c )
{
	// Two bit-identical duplicate triangles, plus a coincident triangle with
	// different content (reversed winding), plus a zero-area triangle.
	std::vector<bvhvec4> verts = {
		{ -2, -2, 4, 0 }, { 2, -2, 4, 0 }, { 0, 2, 4, 0 },		// prim 0
		{ -2, -2, 4, 0 }, { 2, -2, 4, 0 }, { 0, 2, 4, 0 },		// prim 1, exact duplicate
		{ 0, 2, 4, 0 }, { 2, -2, 4, 0 }, { -2, -2, 4, 0 },		// prim 2, same position, reversed
		{ 5, 5, 6, 0 }, { 5, 5, 6, 0 }, { 5, 5, 6, 0 },		// prim 3, zero area
		{ -2, -2, 8, 0 }, { 2, -2, 8, 0 }, { 0, 2, 8, 0 }		// prim 4, further layer
	};
	BVH bvh;
	bvh.Build( verts.data(), 5 );
	const Ray ray( bvhvec3( 0.1f, 0.2f, 0.0f ), bvhvec3( 0.0f, 0.0f, 1.0f ), 20.0f );
	const auto ref = RefBLAS( bvh, ray, 0.0f, ray.hit.t );
	c.check( ref.size() == 4, "duplicates kept, degenerate triangle rejected" );
	c.check( ref[0].prim + ref[1].prim + ref[2].prim == 3, "three distinct prims at the shared depth" );
	CheckAgainstRef( c, bvh, ray, 0.0f, ref, false );

	// The exact duplicates must produce bit-identical t, and the identity
	// tie-break (smaller prim first) must resolve the order without epsilon.
	OrderedHitQuery q( bvh, ray, 0.0f );
	OrderedHit h[3];
	c.check( q.Next( h[0] ) && q.Next( h[1] ) && q.Next( h[2] ), "three coincident hits delivered" );
	c.check( h[0].t == h[1].t, "exact equal distance is not merged" );
	c.check( h[0].prim < h[1].prim, "equal distance resolved by primitive identity" );
}

static void OrderNegativeDirections( ordered_test::Case& c )
{
	const uint32_t N = 96;
	const auto verts = LayeredTris( N );
	BVH bvh;
	bvh.Build( verts.data(), N );
	// Rays traveling toward negative coordinates, including mixed-sign axes.
	const Ray rays[] = {
		Ray( bvhvec3( 0.1f, 0.2f, 120.0f ), bvhvec3( 0.0f, 0.0f, -1.0f ), 200.0f ),
		Ray( bvhvec3( 0.5f, -0.5f, 120.0f ), bvhvec3( -0.05f, 0.04f, -1.2f ), 200.0f ),
		Ray( bvhvec3( 0.0f, 0.0f, 60.0f ), bvhvec3( -0.0f, -0.0f, -1.0f ), 200.0f )
	};
	for (const Ray& ray : rays)
	{
		const auto ref = RefBLAS( bvh, ray, 0.0f, ray.hit.t );
		c.check( ref.size() > 50, "negative-direction ray crosses many layers" );
		// t must still increase along the ray even though world z decreases.
		for (size_t i = 1; i < ref.size(); i++)
			c.check( ref[i - 1].t < ref[i].t, "reference t increases for a -z ray" );
		CheckAgainstRef( c, bvh, ray, 0.0f, ref, false );
	}
}

static void OrderDoublePath( ordered_test::Case& c )
{
	const uint32_t N = 64;
	std::vector<bvhdbl3> verts( (size_t)N * 3 );
	for (uint32_t i = 0; i < N; i++)
	{
		const double z = (double)(i + 1);
		verts[i * 3 + 0] = bvhdbl3( -10, -10, z );
		verts[i * 3 + 1] = bvhdbl3( 10, -10, z );
		verts[i * 3 + 2] = bvhdbl3( 0, 10, z );
	}
	BVH_Double bvh;
	bvh.Build( verts.data(), N );
	const RayEx ray( bvhdbl3( 0.1, 0.2, 0.0 ), bvhdbl3( 0.02, -0.01, 1.2 ), 100.0 );

	std::vector<RefHit> ref;
	for (uint64_t prim = 0; prim < N; prim++)
	{
		double t, u, v;
		if (RefTriD( ray.O, ray.D, 0.0, ray.hit.t,
			verts[prim * 3], verts[prim * 3 + 1], verts[prim * 3 + 2], t, u, v ))
			ref.push_back( RefHit{ t, u, v, 0, (uint64_t)prim } );
	}
	std::sort( ref.begin(), ref.end(), RefLess );

	OrderedHitQueryEx q( bvh, ray, 0.0 );
	OrderedHitEx h;
	size_t count = 0;
	while (q.Next( h ))
	{
		c.check( count < ref.size(), "double query does not over-produce" );
		const RefHit& r = ref[count];
		c.check( h.prim == (uint64_t)r.prim && h.inst == 0, "double identity widths" );
		c.check( CloseD( h.t, r.t ) && CloseD( h.u, r.u ) && CloseD( h.v, r.v ), "double precision tolerance" );
		count++;
	}
	c.check( count == ref.size(), "double query hit count" );

	// Same geometry behind a double-precision TLAS: two instances with a
	// non-uniform scale and a translation, one masked off.
	BVH_Double blas2;
	blas2.Build( verts.data(), N );
	BVH_Double* blassesEx[] = { &blas2 };
	BLASInstanceEx exInst[2] = { BLASInstanceEx( 0 ), BLASInstanceEx( 0 ) };
	exInst[0].transform[0] = 1.5; exInst[0].transform[5] = 2.0; exInst[0].transform[10] = 3.0;
	exInst[0].mask = 1;
	exInst[1].transform[11] = 80.0;
	exInst[1].mask = 2; // skipped by the mask-1 ray below
	BVH_Double tlasEx;
	tlasEx.Build( exInst, 2, blassesEx, 1 );
	{
		const RayEx tray( bvhdbl3( 0.1, 0.2, 0.0 ), bvhdbl3( 0.0, 0.0, 1.0 ), 500.0, 1 );
		const auto tref = RefTLASEx( tlasEx, tray, 0.0, tray.hit.t );
		c.check( !tref.empty(), "double TLAS reference is non-empty" );
		OrderedHitQueryEx tq( tlasEx, tray, 0.0 );
		OrderedHitEx th;
		size_t tn = 0;
		while (tq.Next( th ))
		{
			c.check( tn < tref.size(), "double TLAS count" );
			const RefHit& r = tref[tn];
			c.check( th.inst == (uint64_t)r.inst && th.prim == (uint64_t)r.prim, "double TLAS identity" );
			c.check( CloseD( th.t, r.t ) && CloseD( th.u, r.u ) && CloseD( th.v, r.v ), "double TLAS tolerance" );
			tn++;
		}
		c.check( tn == tref.size(), "double TLAS hits fully delivered" );
	}

	// Double-precision SBVH tree (spatial splits): dedup and ordering must work
	// with the 64-bit index path as well.
	{
		std::vector<bvhdbl3> hqVerts;
		hqVerts.reserve( 400 * 3 );
		auto addD = [&]( bvhdbl3 a, bvhdbl3 b, bvhdbl3 d ) { hqVerts.push_back( a ), hqVerts.push_back( b ), hqVerts.push_back( d ); };
		for (uint32_t i = 0; i < 64; i++)
		{
			const double z = 1.0 + i * 0.5;
			addD( bvhdbl3( -20, -0.1, z ), bvhdbl3( 20, -0.1, z + 2 ), bvhdbl3( 0, 0.1, z + 1 ) );
		}
		for (uint32_t i = 64; i < 400; i++)
		{
			const double side = (i & 1) ? -15.0 : 15.0;
			const double z = 2.0 + (i % 16);
			addD( bvhdbl3( side - 1.5, -1.5, z ), bvhdbl3( side + 1.5, -1.5, z ), bvhdbl3( side, 1.5, z + 0.1 ) );
		}
		BVH_Double hqBVH;
		hqBVH.settings.binnedBVH = true;
		hqBVH.BuildHQ( hqVerts.data(), 400 );
		c.check( hqBVH.idxCount > hqBVH.triCount, "double SBVH reserved fragment slack" );
		// Same per-leaf evidence as the single-precision case, with 64-bit ids.
		std::unordered_map<uint64_t, uint32_t> dLeafRefs;
		uint32_t dDup = 0;
		for (uint64_t n = 0; n < hqBVH.usedNodes; n++)
		{
			const auto& nd = hqBVH.bvhNode[n];
			if (!nd.isLeaf()) continue;
			for (uint64_t k = 0; k < nd.triCount; k++)
				if (++dLeafRefs[hqBVH.primIdx[nd.leftFirst + k]] == 2) dDup++;
		}
		c.check( dDup > 0, "double SBVH put a primitive in two leaves" );
		const RayEx hray( bvhdbl3( 0.0, 0.0, 0.0 ), bvhdbl3( 0.0, 0.0, 1.0 ), 200.0 );
		std::vector<RefHit> href;
		for (uint64_t prim = 0; prim < hqBVH.triCount; prim++)
		{
			double t, u, v;
			if (RefTriD( hray.O, hray.D, 0.0, hray.hit.t,
				hqBVH.verts[prim * 3], hqBVH.verts[prim * 3 + 1], hqBVH.verts[prim * 3 + 2], t, u, v ))
				href.push_back( RefHit{ t, u, v, 0, prim } );
		}
		std::sort( href.begin(), href.end(), RefLess );
		OrderedHitQueryEx hq( hqBVH, hray, 0.0 );
		OrderedHitEx hh;
		uint64_t hn = 0;
		while (hq.Next( hh ))
		{
			c.check( hn < href.size(), "double HQ count" );
			c.check( hh.prim == (uint64_t)href[hn].prim, "double HQ identity and dedup" );
			c.check( CloseD( hh.t, href[hn].t ), "double HQ tolerance" );
			hn++;
		}
		c.check( hn == href.size(), "double HQ fully consumed" );
	}
}

// ============================================================================
// instances group
// ============================================================================

static void InstancesInterleaved( ordered_test::Case& c )
{
	// Instance A: its box is entered first (a vertical triangle straddling the
	// ray near z=1..3, which the ray misses), but its actual hit is at z=90.
	// Instance B: compact box entered around z=10, hit at z=11.
	std::vector<bvhvec4> aVerts = {
		{ 1, 0, 1, 0 }, { 5, -4, 1, 0 }, { -4, 5, 3, 0 },		// plane x+y=1: ray (x+y=0.3) misses
		{ -10, -10, 90, 0 }, { 10, -10, 90, 0 }, { 0, 10, 90, 0 }
	};
	std::vector<bvhvec4> bVerts = {
		{ -10, -10, 11, 0 }, { 10, -10, 11, 0 }, { 0, 10, 11, 0 }
	};
	BVH blasA, blasB;
	blasA.Build( aVerts.data(), 2 );
	blasB.Build( bVerts.data(), 1 );
	BVHBase* blasses[] = { &blasA, &blasB };
	BLASInstance inst[2] = { BLASInstance( 0 ), BLASInstance( 1 ) };
	BVH tlas;
	tlas.Build( inst, 2, blasses, 2 );
	c.check( tlas.aabbMin.z < 5.0f, "instance A box opens before instance B geometry" );

	const Ray ray( bvhvec3( 0.1f, 0.2f, 0.0f ), bvhvec3( 0.0f, 0.0f, 1.0f ), 200.0f );
	const auto ref = RefTLAS( tlas, ray, 0.0f, ray.hit.t );
	c.check( ref.size() == 2, "both instances are eventually hit" );
	c.check( ref[0].inst == 1 && ref[1].inst == 0, "near-triangle instance is delivered first" );
	CheckAgainstRef( c, tlas, ray, 0.0f, ref, true );
}

static void InstancesSharedBlasAffineMask( ordered_test::Case& c )
{
	// One BLAS reused by four instances: scale, shear, reflection and a plain
	// translation; masks select a subset.
	std::vector<bvhvec4> blasVerts = LayeredTris( 8 );
	BVH blas;
	blas.Build( blasVerts.data(), 8 );
	BVHBase* blasses[] = { &blas };
	BLASInstance inst[4] = { BLASInstance( 0 ), BLASInstance( 0 ), BLASInstance( 0 ), BLASInstance( 0 ) };
	float m0[16] = { 2, 0, 0, 0, 0, 3, 0, 0, 0, 0, 4, 0, 0, 0, 0, 1 }; // non-uniform scale
	float m1[16] = { 1, 0.3f, 0, 1, 0, 1, 0, 2, 0, 0, 1, 0, 0, 0, 0, 1 }; // shear + translate (1,2,0)
	float m2[16] = { -1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 30, 0, 0, 0, 1 }; // reflection + z=30
	float m3[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 60, 0, 0, 0, 1 }; // translation z=60
	for (uint32_t k = 0; k < 16; k++)
	{
		inst[0].transform[k] = m0[k]; inst[1].transform[k] = m1[k];
		inst[2].transform[k] = m2[k]; inst[3].transform[k] = m3[k];
	}
	inst[0].mask = 1; inst[1].mask = 1; inst[2].mask = 2; inst[3].mask = 2;
	BVH tlas;
	tlas.Build( inst, 4, blasses, 1 );

	for (uint32_t mask : { 1u, 2u, 3u })
	{
		const Ray ray( bvhvec3( 0.1f, 0.2f, -5.0f ), bvhvec3( 0.0f, 0.0f, 1.0f ), 500.0f, mask );
		const auto ref = RefTLAS( tlas, ray, 0.0f, ray.hit.t );
		c.check( !ref.empty(), "mask selects instances" );
		for (const auto& h : ref)
			c.check( (tlas.instList[h.inst].mask & mask) != 0, "reference and query share the mask rule" );
		CheckAgainstRef( c, tlas, ray, 0.0f, ref, true );
	}
}

static void InstancesHQSplitDedup( ordered_test::Case& c )
{
	// Long sliver triangles spanning the domain plus clusters at both ends
	// force the binned SBVH builder to spatially split a primitive.
	const uint32_t N = 400;
	std::vector<bvhvec4> verts( (size_t)N * 3 );
	uint32_t put = 0;
	auto addTri = [&]( const bvhvec4& a, const bvhvec4& b, const bvhvec4& d ) {
		verts[put * 3] = a; verts[put * 3 + 1] = b; verts[put * 3 + 2] = d; put++;
	};
	for (uint32_t i = 0; i < 64; i++)
	{
		// Long thin strip spanning the whole x domain with a real z tilt.
		const float z = 1.0f + i * 0.5f;
		addTri( bvhvec4( -20, -0.1f, z, 0 ), bvhvec4( 20, -0.1f, z + 2.0f, 0 ), bvhvec4( 0, 0.1f, z + 1.0f, 0 ) );
	}
	for (uint32_t i = 64; i < N; i++)
	{
		// Dense clusters on both ends create overlap that rewards splitting.
		const float side = (i & 1) ? -15.0f : 15.0f;
		const float z = 2.0f + (i % 16);
		addTri( bvhvec4( side - 1.5f, -1.5f, z, 0 ), bvhvec4( side + 1.5f, -1.5f, z, 0 ), bvhvec4( side, 1.5f, z + 0.1f, 0 ) );
	}
	BVH blas;
	blas.settings.binnedBVH = true;
	blas.BuildHQ( verts.data(), N );
	c.check( blas.idxCount > blas.triCount, "SBVH reserved fragment slack" );

	// Count, per leaf, how often each original prim is referenced.
	std::unordered_map<uint32_t, uint32_t> leafRefs;
	uint32_t duplicated = 0;
	for (uint32_t n = 0; n < blas.usedNodes; n++)
	{
		const auto& node = blas.bvhNode[n];
		if (!node.isLeaf()) continue;
		for (uint32_t k = 0; k < node.triCount; k++)
		{
			const uint32_t prim = blas.primIdx[node.leftFirst + k];
			if (++leafRefs[prim] == 2) duplicated++;
		}
	}
	c.check( duplicated > 0, "a primitive really occurs in two different leaves" );

	// Two instances share the split BLAS; each instance's hits stay distinct.
	BVHBase* blasses[] = { &blas };
	BLASInstance inst[2] = { BLASInstance( 0 ), BLASInstance( 0 ) };
	inst[1].transform[11] = 40.0f;
	BVH tlas;
	tlas.Build( inst, 2, blasses, 1 );
	const Ray ray( bvhvec3( 0.0f, 0.0f, 0.0f ), bvhvec3( 0.0f, 0.0f, 1.0f ), 200.0f );
	const auto ref = RefTLAS( tlas, ray, 0.0f, ray.hit.t );
	c.check( !ref.empty(), "split BLAS is traceable" );
	CheckAgainstRef( c, tlas, ray, 0.0f, ref, true );

	// Per-instance dedup: no (inst, prim) pair is delivered twice.
	OrderedHitQuery q( tlas, ray, 0.0f );
	OrderedHit h;
	std::unordered_map<uint64_t, uint32_t> seen;
	uint64_t delivered = 0;
	while (q.Next( h ))
	{
		const uint64_t key = ((uint64_t)h.inst << 32) | h.prim;
		c.check( seen[key]++ == 0, "split primitive delivered once per instance" );
		delivered++;
	}
	c.check( delivered == ref.size(), "no instance lost its hits to dedup" );
}

// ============================================================================
// frontier group
// ============================================================================

static void FrontierEarlyStopBudgets( ordered_test::Case& c )
{
	// BLAS: 4096 separated triangles; two Next() calls must test very few tris.
	const uint32_t N = 4096;
	const auto verts = LayeredTris( N );
	BVH bvh;
	bvh.Build( verts.data(), N );
	{
		const Ray ray( bvhvec3( 0.1f, 0.2f, 0.0f ), bvhvec3( 0.0f, 0.0f, 1.0f ), 100000.0f );
		OrderedHitQuery q( bvh, ray, 0.0f );
		OrderedHit h;
		c.check( q.Next( h ) && h.t == 1.0f && h.prim == 0, "first BLAS layer" );
		c.check( q.Next( h ) && h.t == 2.0f && h.prim == 1, "second BLAS layer" );
		const auto s = q.GetStats();
		c.check( s.triangleTests <= 64, "two-hit triangle test budget" );
		c.check( s.emittedHits == 2 && s.instanceEntries == 0, "BLAS counters" );
		// Destroying the query here is the early-stop path; then a fresh query
		// fully consumes the same tree.
		OrderedHitQuery full( bvh, ray, 0.0f );
		uint64_t total = 0;
		while (full.Next( h )) total++;
		c.check( total == N, "full BLAS consumption" );
		const auto fs = full.GetStats();
		// The axis ray reaches every leaf, so every reachable node is expanded
		// and every original triangle is tested exactly once. NodeCount() is
		// the reachable count (usedNodes additionally reserves an unused node).
		c.check( fs.nodeVisits == (uint64_t)bvh.NodeCount(), "each reachable BLAS node expanded exactly once" );
		c.check( fs.triangleTests == (uint64_t)bvh.triCount, "each BLAS primitive tested exactly once" );
		c.check( fs.emittedHits == N, "emitted counter matches" );
	}

	// TLAS: 4096 single-triangle instances sharing one BLAS.
	{
		std::vector<bvhvec4> one = { { -10, -10, 0, 0 }, { 10, -10, 0, 0 }, { 0, 10, 0, 0 } };
		BVH blas;
		blas.Build( one.data(), 1 );
		std::vector<BLASInstance> inst( N );
		for (uint32_t i = 0; i < N; i++)
		{
			inst[i] = BLASInstance( 0 );
			inst[i].transform[11] = (float)(i + 1); // local z=0 lands on world z=i+1
		}
		BVH* blasses[] = { &blas };
		BVH tlas;
		tlas.Build( inst.data(), N, blasses, 1 );
		const Ray ray( bvhvec3( 0.1f, 0.2f, 0.0f ), bvhvec3( 0.0f, 0.0f, 1.0f ), 100000.0f );
		OrderedHitQuery q( tlas, ray, 0.0f );
		OrderedHit h;
		c.check( q.Next( h ) && h.inst == 0, "first TLAS instance" );
		c.check( q.Next( h ) && h.inst == 1, "second TLAS instance" );
		const auto s = q.GetStats();
		c.check( s.instanceEntries <= 64, "two-hit instance entry budget" );
		c.check( s.triangleTests <= 64, "instance triangles are not all tested" );
		uint64_t total = 0;
		uint32_t lastInst = 0;
		while (q.Next( h ))
		{
			c.check( h.inst >= lastInst, "remaining instances stay ordered" );
			lastInst = h.inst;
			total++;
		}
		c.check( total + 2 == N, "full TLAS consumption" );
		const auto fs = q.GetStats();
		c.check( fs.instanceEntries == N, "every mask-passing instance entered once" );
		// Every instance box is intersected by the axis ray, so the TLAS tree
		// and every single-node BLAS are expanded in full.
		c.check( fs.nodeVisits == (uint64_t)tlas.NodeCount() + (uint64_t)N * blas.NodeCount(),
			"each (inst, BLAS node) and TLAS node expanded exactly once" );
		c.check( fs.triangleTests == N, "each instance triangle tested exactly once" );
	}
}

static void FrontierInterleavedAndThreaded( ordered_test::Case& c )
{
	const auto verts = LayeredTris( 128 );
	BVH bvh;
	bvh.Build( verts.data(), 128 );
	const Ray r1( bvhvec3( 0.1f, 0.2f, 0.0f ), bvhvec3( 0.0f, 0.0f, 1.0f ), 1000.0f );
	// Direction chosen to stay well inside every triangle, avoiding edges
	// where float and double rounding could legitimately disagree.
	const Ray r2( bvhvec3( -0.3f, 0.4f, 0.0f ), bvhvec3( 0.07f, 0.03f, 1.0f ), 1000.0f );
	const auto ref1 = RefBLAS( bvh, r1, 0.0f, r1.hit.t );
	const auto ref2 = RefBLAS( bvh, r2, 0.0f, r2.hit.t );

	// Two queries over the same read-only scene, advanced alternately.
	OrderedHitQuery q1( bvh, r1, 0.0f ), q2( bvh, r2, 0.0f );
	std::vector<OrderedHit> g1, g2;
	OrderedHit a, b;
	bool more1 = true, more2 = true;
	while (more1 || more2)
	{
		if (more1) { more1 = q1.Next( a ); if (more1) g1.push_back( a ); }
		if (more2) { more2 = q2.Next( b ); if (more2) g2.push_back( b ); }
	}
	c.check( g1.size() == ref1.size() && g2.size() == ref2.size(),
		"interleaved queries complete" );
	for (size_t i = 0; i < g1.size(); i++)
		c.check( g1[i].prim == (uint32_t)ref1[i].prim && CloseF( g1[i].t, (float)ref1[i].t ), "interleaved query 1" );
	for (size_t i = 0; i < g2.size(); i++)
		c.check( g2[i].prim == (uint32_t)ref2[i].prim && CloseF( g2[i].t, (float)ref2[i].t ), "interleaved query 2" );

	// Same scene queried concurrently from two threads.
	std::vector<OrderedHit> t1, t2;
	std::thread th( [&]() {
		OrderedHitQuery q( bvh, r1, 0.0f ); OrderedHit h;
		while (q.Next( h )) t1.push_back( h );
	} );
	{
		OrderedHitQuery q( bvh, r2, 0.0f ); OrderedHit h;
		while (q.Next( h )) t2.push_back( h );
	}
	th.join();
	c.check( t1.size() == ref1.size() && t2.size() == ref2.size(), "threaded queries complete" );

	// An ended query stays ended and never overwrites the result object.
	OrderedHitQuery done( bvh, r1, 0.0f );
	OrderedHit h;
	while (done.Next( h )) {}
	const OrderedHit sentinel{ 123.0f, 0.5f, 0.25f, 777u, 888u };
	OrderedHit probe = sentinel;
	c.check( done.Next( probe ) == false, "Next reports completion" );
	c.check( probe.t == 123.0f && probe.prim == 777u && probe.inst == 888u, "ended Next leaves the result untouched" );
	c.check( done.Next( probe ) == false, "repeated Next stays false" );
	c.check( done.GetStats().emittedHits == ref1.size(), "counters freeze at completion" );

	// Empty, never-built BVH is an empty query.
	BVH empty;
	OrderedHitQuery eq( empty, r1, 0.0f );
	OrderedHit eh{ 1, 1, 1, 1, 1 };
	c.check( eq.Next( eh ) == false && eh.prim == 1, "empty BLAS query" );
	c.check( eq.GetStats().nodeVisits == 0, "empty query does no work" );
}

static void FrontierAxisRaysAndIntervals( ordered_test::Case& c )
{
	// Plates in several orientations so axis-parallel rays with signed zeros
	// actually meet box boundaries head-on.
	std::vector<bvhvec4> verts = {
		{ -4, -4, 3, 0 }, { 4, -4, 3, 0 }, { 0, 4, 3, 0 },		// horizontal
		{ -4, -4, 6, 0 }, { 4, -4, 6, 0 }, { 0, 4, 6, 0 },
		{ 0, -4, 1, 0 }, { 0, -4, 9, 0 }, { 0, 4, 5, 0 }			// vertical plane x=0
	};
	BVH bvh;
	bvh.Build( verts.data(), 3 );
	const bvhvec3 origins[] = { bvhvec3( 0, 0, 0 ), bvhvec3( 0.1f, -0.2f, 4.0f ), bvhvec3( -1, 1, 2 ) };
	const bvhvec3 dirs[] = {
		bvhvec3( 0.0f, 0.0f, 1.0f ), bvhvec3( -0.0f, -0.0f, 1.0f ),
		bvhvec3( 1.0f, -0.0f, 0.0f ), bvhvec3( -0.0f, 1.0f, -0.0f )
	};
	for (const bvhvec3& O : origins) for (const bvhvec3& D : dirs)
	{
		const Ray ray( O, D, 50.0f );
		const auto ref = RefBLAS( bvh, ray, 0.0f, ray.hit.t );
		CheckAgainstRef( c, bvh, ray, 0.0f, ref, false );
	}
	// Ray originating inside a root box, with tmin equal to the first hit.
	// Origin z=3.5; the horizontal triangle at world z=6 is reached at t=2.5.
	const Ray inside( bvhvec3( 0.0f, 0.0f, 3.5f ), bvhvec3( 0.0f, 0.0f, 1.0f ), 50.0f );
	const auto refIn = RefBLAS( bvh, inside, 2.5f, 50.0f );
	c.check( !refIn.empty() && refIn.front().t == 2.5, "in-box ray with tmin on a hit" );
	CheckAgainstRef( c, bvh, inside, 2.5f, refIn, false );

	// Empty interval and tmin == tmax produce nothing, without false hits.
	const Ray capped( bvhvec3( 0, 0, 0 ), bvhvec3( 0, 0, 1 ), 3.0f );
	OrderedHitQuery q( bvh, capped, 3.0f );
	OrderedHit h;
	c.check( q.Next( h ) == false, "half-open empty interval" );
}

// ============================================================================
// render group
// ============================================================================

static void RenderPpmPixels( ordered_test::Case& c )
{
	// The fixed driver generates the image at .cache/ordered first; a CTest
	// fixture may point the test at another location via this environment path.
	const char* path = std::getenv( "ORDERED_LAYERS_PPM" );
	const std::string imagePath = path ? path : ".cache/ordered/ordered_layers.ppm";
	std::ifstream f( imagePath );
	c.check( f.good(), ( "example image exists: " + imagePath ).c_str() );
	if (!f.good()) return;
	std::string tok;
	std::vector<int> vals;
	while (f >> tok)
	{
		if (tok[0] == 'P') { c.check( tok == "P3", "ASCII P3 magic" ); continue; }
		try { vals.push_back( std::stoi( tok ) ); }
		catch (...) { c.check( false, "image token is numeric" ); return; }
	}
	c.check( vals.size() >= 3, "image has a header" );
	c.check( vals[0] == 32 && vals[1] == 32 && vals[2] == 255, "32x32, 8-bit PPM" );
	c.check( vals.size() == 3 + 32 * 32 * 3, "exact pixel payload" );
	int bgLike = 0, colored = 0;
	const int bg[3] = { vals[3], vals[4], vals[5] }; // top-left pixel is the background
	bool multiColor = false;
	int firstColor[3] = { -1, -1, -1 };
	for (int p = 0; p < 32 * 32; p++)
	{
		const int* px = &vals[3 + p * 3];
		for (int k = 0; k < 3; k++) c.check( px[k] >= 0 && px[k] <= 255, "channel range" );
		const bool isBg = px[0] == bg[0] && px[1] == bg[1] && px[2] == bg[2];
		if (isBg) bgLike++; else
		{
			colored++;
			if (firstColor[0] < 0) { for (int k = 0; k < 3; k++) firstColor[k] = px[k]; }
			else if (px[0] != firstColor[0] || px[1] != firstColor[1] || px[2] != firstColor[2]) multiColor = true;
		}
	}
	c.check( bgLike > 0, "background pixels remain visible" );
	c.check( colored > 32, "a substantial part of the image shows surfaces" );
	c.check( multiColor, "intersecting colored layers produce multiple colors" );
}

static void RenderAlphaEarlyStop( ordered_test::Case& c )
{
	// Ten identical full-cover translucent layers, alpha 0.5: front-to-back
	// transmittance is 0.5^n, which drops below 0.01 after seven layers.
	const uint32_t N = 10;
	const auto verts = LayeredTris( N );
	BVH bvh;
	bvh.Build( verts.data(), N );
	const Ray ray( bvhvec3( 0.1f, 0.2f, 0.0f ), bvhvec3( 0.0f, 0.0f, 1.0f ), 100.0f );
	OrderedHitQuery q( bvh, ray, 0.0f );
	OrderedHit h;
	c.check( q.Next( h ) && h.prim == 0, "first hit identity" );
	c.check( q.Next( h ) && h.prim == 1, "second hit identity" );
	int consumed = 2;
	float transmittance = 1.0f;
	const float alpha = 0.5f;
	transmittance *= (1.0f - alpha) * (1.0f - alpha); // first two layers
	while (transmittance >= 0.01f)
	{
		c.check( q.Next( h ), "a layer is available while opacity accumulates" );
		transmittance *= (1.0f - alpha);
		consumed++;
	}
	c.check( consumed == 7, "compositing stops after the seventh layer" );
	c.check( transmittance < 0.01f, "transmittance threshold crossed" );
	c.check( q.Next( h ), "the query itself is still alive after the renderer stopped" );

	// A ray that misses everything leaves the background untouched: no hits.
	const Ray miss( bvhvec3( 50.0f, 50.0f, 0.0f ), bvhvec3( 0.0f, 0.0f, 1.0f ), 100.0f );
	OrderedHitQuery mq( bvh, miss, 0.0f );
	OrderedHit mh;
	c.check( mq.Next( mh ) == false, "background rays deliver no hits" );
}

void register_ordered_tests( ordered_test::Registry& reg )
{
	reg.add( "order", "order_blas_reference", OrderBlasReference );
	reg.add( "order", "order_indexed_stride", OrderIndexedAndStride );
	reg.add( "order", "order_equal_distance", OrderEqualDistance );
	reg.add( "order", "order_negative_directions", OrderNegativeDirections );
	reg.add( "order", "order_double_path", OrderDoublePath );
	reg.add( "instances", "instances_interleaved", InstancesInterleaved );
	reg.add( "instances", "instances_shared_blas_affine_mask", InstancesSharedBlasAffineMask );
	reg.add( "instances", "instances_hq_split_dedup", InstancesHQSplitDedup );
	reg.add( "frontier", "frontier_early_stop_budgets", FrontierEarlyStopBudgets );
	reg.add( "frontier", "frontier_interleaved_threaded", FrontierInterleavedAndThreaded );
	reg.add( "frontier", "frontier_axis_rays_intervals", FrontierAxisRaysAndIntervals );
	reg.add( "render", "render_ppm_pixels", RenderPpmPixels );
	reg.add( "render", "render_alpha_early_stop", RenderAlphaEarlyStop );
}
