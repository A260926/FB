/*
The MIT License (MIT)

Copyright (c) 2024-2026, Jacco Bikker / Breda University of Applied Sciences.

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
*/

// Ordered, pausable hit queries for TinyBVH.
//
// Include tiny_bvh.h (with TINYBVH_IMPLEMENTATION in exactly one translation
// unit) and this header to get:
//
//   tinybvh::OrderedHitQuery    for tinybvh::BVH        / tinybvh::Ray
//   tinybvh::OrderedHitQueryEx  for tinybvh::BVH_Double / tinybvh::RayEx
//
// A query is constructed over a const BVH and a const Ray, with a tmin of the
// same precision as the ray; the upper bound is ray.hit.t at construction.
// Next() delivers one hit at a time, in front-to-back order, until the query
// is exhausted; the caller may stop and destroy the query at any point. The
// query only reads the scene and keeps all working state to itself, so several
// queries may be interleaved or run concurrently on the same scene.

#ifndef TINY_BVH_ORDERED_H_
#define TINY_BVH_ORDERED_H_

#include "tiny_bvh.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <queue>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace tinybvh {

// Cumulative counters for an OrderedHitQuery, counted since construction.
struct OrderedQueryStats
{
	std::uint64_t nodeVisits = 0;		// TLAS/BLAS nodes actually expanded
	std::uint64_t triangleTests = 0;	// original triangles intersected, once per (inst,prim)
	std::uint64_t instanceEntries = 0;	// mask-passing instances entered
	std::uint64_t emittedHits = 0;		// hits handed to the caller via Next()
};

// One delivered hit. t is a world-ray parameter even for TLAS instances.
// inst and prim are always two independent fields: inst is 0 for a plain
// BLAS, and for a TLAS it is the index into the input instance array.
template <typename Float, typename Index>
struct OrderedHitT
{
	Float t = Float( 0 ), u = Float( 0 ), v = Float( 0 );
	Index prim = Index( 0 );
	Index inst = Index( 0 );
};

using OrderedHit = OrderedHitT<float, uint32_t>;
using OrderedHitEx = OrderedHitT<double, uint64_t>;

namespace ordered_detail {

// Slab test for a ray whose reciprocal direction may contain +/-BVH_RCP_FAR
// for zero components (axis-parallel rays, signed zero included). Reports the
// first point along the ray inside [tmin,tmax) at which the box is active.
template <typename Float, typename Vec3>
inline bool OrderedSlab( const Vec3& O, const Vec3& rD, const Vec3& bmin, const Vec3& bmax,
	const Float tmin, const Float tmax, Float& tenter )
{
	Float t0 = -std::numeric_limits<Float>::infinity();
	Float t1 = std::numeric_limits<Float>::infinity();
	for (int axis = 0; axis < 3; axis++)
	{
		const Float tx1 = (bmin[axis] - O[axis]) * rD[axis];
		const Float tx2 = (bmax[axis] - O[axis]) * rD[axis];
		t0 = tinybvh_max( t0, tinybvh_min( tx1, tx2 ) );
		t1 = tinybvh_min( t1, tinybvh_max( tx1, tx2 ) );
	}
	// t1 < t0: ray misses the box; t1 < tmin: box is entirely behind the query interval.
	if (t1 < t0 || t1 < tmin) return false;
	const Float enter = tinybvh_max( t0, tmin ); // a box containing the origin opens at tmin.
	if (!(enter < tmax)) return false;           // half-open upper bound
	tenter = enter;
	return true;
}

// Double-sided Moeller-Trumbore test, matching the library semantics:
// a zero-area triangle (a == 0) never hits and the interval is [tmin,tmax).
template <typename Float, typename Vec3>
inline bool OrderedTriangle( const Vec3& O, const Vec3& D, const Vec3& v0, const Vec3& v1, const Vec3& v2,
	const Float tmin, const Float tmax, Float& t, Float& u, Float& v )
{
	const Vec3 e1 = v1 - v0, e2 = v2 - v0;
	const Vec3 h = tinybvh_cross( D, e2 );
	const Float a = tinybvh_dot( e1, h );
	if (a == Float( 0 )) return false;
	const Float f = Float( 1 ) / a;
	const Vec3 s = O - v0;
	u = f * tinybvh_dot( s, h );
	if (!(u >= Float( 0 ) && u <= Float( 1 ))) return false;
	const Vec3 q = tinybvh_cross( s, e1 );
	v = f * tinybvh_dot( D, q );
	if (!(v >= Float( 0 ) && v <= Float( 1 ) && u + v <= Float( 1 ))) return false;
	t = f * tinybvh_dot( e2, q );
	return t >= tmin && t < tmax;
}

template <typename Float, typename Index>
class OrderedHitQueryT
{
public:
	using BVH = impl::BVH<Float, Index>;
	using Ray = impl::Ray<Float, Index>;
	using Node = typename BVH::BVHNode;
	using Vec3 = typename bvh_traits<Float>::vec3;
	using BLASInstance = impl::BLASInstance<Float, Index>;
	using Hit = OrderedHitT<Float, Index>;

	OrderedHitQueryT( const BVH& bvh, const Ray& ray, const Float tmin ) :
		m_top( bvh ), m_worldRay( ray ), m_tmin( tmin ), m_tmax( ray.hit.t )
	{
		// Construction only stores the parameters and initializes root work:
		// no node, primitive or instance arrays are scanned or cleared.
		m_isTLAS = bvh.instList != 0;
		if (bvh.layout != LAYOUT_BVH || bvh.bvhNode == 0 || bvh.triCount == 0) { m_finished = true; return; }
		// A plain BLAS is instance 0 of itself: its local ray is the world ray.
		if (!m_isTLAS)
		{
			InstState state;
			state.localRay = ray;
			m_states.emplace( Index( 0 ), std::move( state ) );
		}
		Float tenter;
		if (OrderedSlab( m_worldRay.O, m_worldRay.rD, bvh.bvhNode[0].aabbMin, bvh.bvhNode[0].aabbMax,
			m_tmin, m_tmax, tenter ))
		{
			m_heap.push( Entry{ &bvh.bvhNode[0], &bvh, m_isTLAS ? SENTINEL : Index( 0 ), tenter, ++m_nextId, false } );
		}
		else m_finished = true; // root box does not overlap the query interval
	}

	OrderedHitQueryT( const OrderedHitQueryT& ) = delete;
	OrderedHitQueryT& operator=( const OrderedHitQueryT& ) = delete;

	// Delivers the next hit in (t,inst,prim) order. Returns false once no more
	// hits can exist; after false the query stays finished and 'out' is untouched.
	bool Next( Hit& out )
	{
		if (m_finished) return false;
		for (;;)
		{
			if (m_candidates.empty())
			{
				if (m_heap.empty()) { m_finished = true; return false; }
				Expand();
				continue;
			}
			const std::size_t best = BestCandidate();
			// Every pending region that could still hold a hit at this distance,
			// or an equal-distance hit with a smaller identity, must be resolved.
			if (!m_heap.empty() && m_heap.top().tenter <= m_candidates[best].t) { Expand(); continue; }
			const Candidate c = m_candidates[best];
			m_candidates[best] = m_candidates.back();
			m_candidates.pop_back();
			out.t = c.t; out.u = c.u; out.v = c.v; out.prim = c.prim; out.inst = c.inst;
			m_stats.emittedHits++;
			return true;
		}
	}

	OrderedQueryStats GetStats() const { return m_stats; }

private:
	static constexpr Index SENTINEL = Index( ~Index( 0 ) );

	struct Entry
	{
		const Node* node;	// BVH node to expand; null for a not-yet-entered instance
		const BVH* tree;	// tree owning the node: the TLAS or a BLAS
		Index inst;			// instance index for BLAS work, SENTINEL for TLAS nodes
		Float tenter;		// earliest hit the region can produce, in world-ray t
		std::uint64_t id;	// stable tie-break among identical keys
		bool pendingInst;	// entry: enter instance 'inst' when this reaches the top
	};
	struct EntryCmp
	{
		bool operator()( const Entry& a, const Entry& b ) const
		{
			if (a.tenter != b.tenter) return a.tenter > b.tenter; // min-heap on tenter
			if (a.inst != b.inst) return a.inst > b.inst;
			return a.id > b.id;
		}
	};
	struct Candidate { Float t, u, v; Index prim, inst; };
	static_assert( sizeof( Candidate ) == sizeof( Hit ) );
	struct InstState
	{
		Ray localRay;						  // ray in the BLAS frame
		std::unordered_set<Index> testedPrims; // one tri test per (inst,prim)
	};

	const Ray& RayFor( const Entry& e )
	{
		return e.inst == SENTINEL ? m_worldRay : m_states.find( e.inst )->second.localRay;
	}

	InstState& EnterInstance( const Index instIdx )
	{
		auto it = m_states.find( instIdx );
		if (it != m_states.end()) return it->second;
		const BLASInstance& inst = m_top.instList[instIdx];
		InstState state;
		state.localRay.O = tinybvh_transform_point( m_worldRay.O, inst.invTransform );
		state.localRay.D = tinybvh_transform_vector( m_worldRay.D, inst.invTransform );
		state.localRay.rD = tinybvh_rcp( state.localRay.D );
		state.localRay.mask = m_worldRay.mask;
		auto inserted = m_states.emplace( instIdx, std::move( state ) );
		m_stats.instanceEntries++;
		return inserted.first->second;
	}

	void PushChild( const Node* child, const BVH* tree, const Index inst, const Ray& ray )
	{
		Float tenter;
		if (OrderedSlab( ray.O, ray.rD, child->aabbMin, child->aabbMax, m_tmin, m_tmax, tenter ))
			m_heap.push( Entry{ child, tree, inst, tenter, ++m_nextId, false } );
	}

	// Pops the earliest pending region and processes it.
	void Expand()
	{
		const Entry e = m_heap.top();
		m_heap.pop();
		if (e.pendingInst)
		{
			// Lazily enter the instance only once its region reaches the frontier:
			// mask failures cost nothing, and whole BLASes are never queried ahead
			// of nearer pending work.
			const BLASInstance& inst = m_top.instList[e.inst];
			if (!(inst.mask & m_worldRay.mask)) return;
			const BVH* blas = (const BVH*)m_top.blasList[inst.blasIdx];
			if (blas->layout != LAYOUT_BVH || blas->bvhNode == 0 || blas->triCount == 0) return;
			const InstState& state = EnterInstance( e.inst );
			PushChild( &blas->bvhNode[0], blas, e.inst, state.localRay );
			return;
		}
		m_stats.nodeVisits++;
		const Node* node = e.node;
		if (!node->isLeaf())
		{
			const Node* c1 = &e.tree->bvhNode[node->leftFirst];
			const Node* c2 = &e.tree->bvhNode[node->leftFirst + 1];
			const Ray& ray = RayFor( e );
			PushChild( c1, e.tree, e.inst, ray );
			PushChild( c2, e.tree, e.inst, ray );
			return;
		}
		if (m_isTLAS && e.inst == SENTINEL)
		{
			// TLAS leaf: queue one lazily-entered entry per instance. The instance
			// AABB slab test keeps instances outside the ray interval out of the heap.
			for (Index i = 0; i < node->triCount; i++)
			{
				const Index instIdx = m_top.primIdx[node->leftFirst + i];
				const BLASInstance& inst = m_top.instList[instIdx];
				Float tenter;
				if (!OrderedSlab( m_worldRay.O, m_worldRay.rD, inst.aabbMin, inst.aabbMax,
					m_tmin, m_tmax, tenter )) continue;
				m_heap.push( Entry{ nullptr, nullptr, instIdx, tenter, ++m_nextId, true } );
			}
			return;
		}
		// BLAS leaf: test each original triangle at most once for this instance.
		InstState& state = m_states.find( e.inst )->second;
		const BVH& blas = *e.tree;
		for (Index i = 0; i < node->triCount; i++)
		{
			const Index pi = blas.primIdx[node->leftFirst + i];
			if (!state.testedPrims.insert( pi ).second) continue; // spatial-split duplicate
			Vec3 v0, v1, v2;
			if (blas.vertIdx != 0)
			{
				const Index i0 = blas.vertIdx[pi * 3], i1 = blas.vertIdx[pi * 3 + 1], i2 = blas.vertIdx[pi * 3 + 2];
				v0 = blas.verts[i0], v1 = blas.verts[i1], v2 = blas.verts[i2];
			}
			else
			{
				v0 = blas.verts[pi * 3]; v1 = blas.verts[pi * 3 + 1]; v2 = blas.verts[pi * 3 + 2];
			}
			Float t, u, v;
			m_stats.triangleTests++;
			// The local ray shares the world parameter t, so the world [tmin,tmax)
			// interval applies unchanged.
			if (OrderedTriangle( state.localRay.O, state.localRay.D, v0, v1, v2, m_tmin, m_tmax, t, u, v ))
				m_candidates.push_back( Candidate{ t, u, v, pi, e.inst } );
		}
	}

	std::size_t BestCandidate() const
	{
		return (std::size_t)(std::min_element( m_candidates.begin(), m_candidates.end(),
			[]( const Candidate& a, const Candidate& b )
			{
				if (a.t != b.t) return a.t < b.t;
				if (a.inst != b.inst) return a.inst < b.inst;
				return a.prim < b.prim;
			} ) - m_candidates.begin());
	}

	const BVH& m_top;
	Ray m_worldRay; // query-private copy; the caller's ray is never modified
	const Float m_tmin, m_tmax;
	bool m_isTLAS = false;
	bool m_finished = false;
	std::uint64_t m_nextId = 0;
	OrderedQueryStats m_stats;
	std::priority_queue<Entry, std::vector<Entry>, EntryCmp> m_heap;
	std::vector<Candidate> m_candidates;
	std::unordered_map<Index, InstState> m_states;
};

} // namespace ordered_detail

using OrderedHitQuery = ordered_detail::OrderedHitQueryT<float, uint32_t>;
#ifdef DOUBLE_PRECISION_SUPPORT
using OrderedHitQueryEx = ordered_detail::OrderedHitQueryT<double, uint64_t>;
#endif

} // namespace tinybvh

#endif // TINY_BVH_ORDERED_H_
