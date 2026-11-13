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

// Ordered, pausable hit queries for offline ray tracing.
//
//   #include "tiny_bvh_ordered.h"
//
// tinybvh::OrderedHitQuery / OrderedHitQueryEx expose the same scalar type
// pairs as BVH/Ray and BVH_Double/RayEx. A query is constructed over a const
// BVH and a const Ray; Next() delivers one triangle hit at a time, in
// (t, inst, prim) lexicographic order, until the ray interval is exhausted.
// The caller may stop at any time and simply destroy the query.
//
// Scope: binary BVH trees produced by Build / BuildQuick / BuildHQ /
// BuildSIMD (LAYOUT_BVH), both as plain BLAS and as a single-level TLAS whose
// BLAS instances are themselves LAYOUT_BVH trees. Wide-node layouts, GPUs,
// voxels, custom primitives, nested TLAS, opacity micro maps and motion
// instances are not supported by this header.
//
// The query streams directly through the existing nodes: it keeps a frontier
// of pending TLAS/BLAS nodes ordered by box-entry distance and a heap of
// already-tested candidate hits. No flattened acceleration structure is
// built, no instance or primitive table is cleared, and the full hit list is
// never collected before it is delivered.

#ifndef TINY_BVH_ORDERED_H_
#define TINY_BVH_ORDERED_H_

#include "tiny_bvh.h"

#include <algorithm>
#include <cstdint>
#include <deque>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace tinybvh {

// A single delivered hit. Float/Index widths match the ray type.
// inst and prim are always two independent fields: for a plain BLAS query
// inst is 0; for a TLAS query inst is the instance-array index and prim is
// the original triangle index inside that instance's BLAS.
template <typename Float, typename Index>
struct OrderedHitT
{
	Float t = Float( 0 );
	Float u = Float( 0 );
	Float v = Float( 0 );
	Index prim = Index( 0 );
	Index inst = Index( 0 );
};

using OrderedHit = OrderedHitT<float, uint32_t>;
using OrderedHitEx = OrderedHitT<double, uint64_t>;

// Cumulative counters for a query, measured from construction.
struct OrderedQueryStats
{
	uint64_t nodeVisits = 0;		// TLAS or BLAS nodes actually expanded
	uint64_t triangleTests = 0;		// original triangle intersections, per instance
	uint64_t instanceEntries = 0;	// mask-passing instances entered
	uint64_t emittedHits = 0;		// hits handed to the caller through Next()
};

namespace ordered_detail {

template <typename Float, typename Index>
class OrderedQueryImpl
{
public:
	using BVH = impl::BVH<Float, Index>;
	using BVHNode = typename BVH::BVHNode;
	using Ray = impl::Ray<Float, Index>;
	using Vec3 = typename bvh_traits<Float>::vec3;
	using Instance = impl::BLASInstance<Float, Index>;
	using Hit = OrderedHitT<Float, Index>;

	// tmin uses the same scalar precision as the ray. The upper bound is
	// taken from ray.hit.t at construction time.
	OrderedQueryImpl( const BVH& bvh, const Ray& ray, const Float tmin ) :
		m_TLAS( bvh ), m_O( ray.O ), m_D( ray.D ), m_rD( ray.rD ), m_mask( ray.mask ),
		m_tmin( tmin ), m_tmax( ray.hit.t )
	{
		// Construction only stores parameters and initializes the root work
		// item; no nodes beyond the root box test are inspected here.
		if (bvh.bvhNode == 0 || bvh.triCount == 0) { m_done = true; return; }
		if (!(m_tmin >= Float( 0 )) || !(m_tmin <= m_tmax)) { m_done = true; return; }
		if (m_tmin >= m_tmax) return; // half-open empty interval; root is not needed
		const Float entry = BoxEntry( &bvh.bvhNode[0], m_O, m_rD );
		if (entry == Far() || entry > m_tmax) { m_done = true; return; }
		if (bvh.isTLAS())
		{
			// TLAS nodes travel with a null state and are tested in world space;
			// BLAS states are created as instance leaves are expanded.
			m_frontier.push_back( FrontierItem{ &bvh.bvhNode[0], entry, nullptr } );
		}
		else
		{
			// A plain BLAS is one implicit instance: identity frame, inst 0.
			m_states.emplace_back();
			BLASState* root = &m_states.back();
			root->blas = &bvh;
			root->instId = 0;
			root->O = m_O; root->D = m_D; root->rD = m_rD;
			m_frontier.push_back( FrontierItem{ &bvh.bvhNode[0], entry, root } );
		}
		std::push_heap( m_frontier.begin(), m_frontier.end(), FrontierCmp() );
	}

	// Delivers the next hit. Returns false, without touching 'out', once the
	// query has ended; further calls keep returning false.
	bool Next( Hit& out )
	{
		if (m_done) return false;
		if (m_tmin >= m_tmax) { m_done = true; return false; }
		// Expand every pending region that could still hold a hit strictly
		// closer than, or at the same distance with a smaller identity than,
		// the best candidate currently available.
		while (!m_frontier.empty())
		{
			std::pop_heap( m_frontier.begin(), m_frontier.end(), FrontierCmp() );
			const FrontierItem item = m_frontier.back();
			if (!m_candidates.empty() && item.tenter > m_candidates.front().t)
			{
				// This region cannot beat the pending candidate; keep it for
				// a later Next() call and stop expanding.
				std::push_heap( m_frontier.begin(), m_frontier.end(), FrontierCmp() );
				break;
			}
			m_frontier.pop_back();
			Expand( item );
		}
		if (m_candidates.empty()) { m_done = true; return false; }
		std::pop_heap( m_candidates.begin(), m_candidates.end(), CandidateCmp() );
		const Candidate c = m_candidates.back();
		m_candidates.pop_back();
		m_stats.emittedHits++;
		out.t = c.t;
		out.u = c.u;
		out.v = c.v;
		out.prim = c.prim;
		out.inst = c.state->instId;
		return true;
	}

	OrderedQueryStats GetStats() const { return m_stats; }

private:
	static constexpr Float Far() { return bvh_far<Float>; }

	struct BLASState
	{
		const BVH* blas = 0;
		Index instId = 0;
		Vec3 O{}, D{}, rD{};					// ray in this instance's local frame
		std::unordered_set<Index> testedPrims;	// spatial-split deduplication
	};

	struct FrontierItem
	{
		const BVHNode* node;
		Float tenter;
		BLASState* state; // null: TLAS node in world space; set: BLAS node
	};

	struct Candidate
	{
		Float t, u, v;
		Index prim;
		BLASState* state;
	};

	struct FrontierCmp
	{
		// std::heap is a max-heap; invert so the smallest entry sits on top.
		bool operator()( const FrontierItem& a, const FrontierItem& b ) const { return a.tenter > b.tenter; }
	};

	struct CandidateCmp
	{
		bool operator()( const Candidate& a, const Candidate& b ) const
		{
			// Exact equality: equal-distance hits must never be merged away.
			if (a.t != b.t) return a.t > b.t;
			const Index ai = a.state->instId, bi = b.state->instId;
			if (ai != bi) return ai > bi;
			return a.prim > b.prim;
		}
	};

	// Slab test with finite reciprocal direction components (tinybvh_safercp
	// maps zero components to a finite far reciprocal), so axis-aligned rays
	// and signed zeros do not need a separate path.
	static Float BoxEntry( const BVHNode* node, const Vec3& O, const Vec3& rD )
	{
		Float lo = -Far(), hi = Far();
		const Vec3 mn = node->aabbMin, mx = node->aabbMax;
		Float t1 = (mn.x - O.x) * rD.x, t2 = (mx.x - O.x) * rD.x;
		if (t1 > t2) tinybvh_swap( t1, t2 );
		lo = tinybvh_max( lo, t1 ), hi = tinybvh_min( hi, t2 );
		t1 = (mn.y - O.y) * rD.y, t2 = (mx.y - O.y) * rD.y;
		if (t1 > t2) tinybvh_swap( t1, t2 );
		lo = tinybvh_max( lo, t1 ), hi = tinybvh_min( hi, t2 );
		t1 = (mn.z - O.z) * rD.z, t2 = (mx.z - O.z) * rD.z;
		if (t1 > t2) tinybvh_swap( t1, t2 );
		lo = tinybvh_max( lo, t1 ), hi = tinybvh_min( hi, t2 );
		if (hi < lo) return Far();
		return lo;
	}

	// Moeller-Trumbore, matching the library's scalar test: double-sided,
	// zero-area (a == 0) triangles miss, and the hit interval is [tmin, tmax).
	bool TriHit( const BLASState& state, const BVH& blas, const Index prim, Float& tOut, Float& uOut, Float& vOut ) const
	{
		Index i0, i1, i2;
		if (blas.vertIdx != 0)
		{
			i0 = blas.vertIdx[prim * 3];
			i1 = blas.vertIdx[prim * 3 + 1];
			i2 = blas.vertIdx[prim * 3 + 2];
		}
		else i0 = prim * 3, i1 = prim * 3 + 1, i2 = prim * 3 + 2;
		const Vec3 v0 = blas.verts[i0];
		const Vec3 e1 = Vec3( blas.verts[i1] ) - v0;
		const Vec3 e2 = Vec3( blas.verts[i2] ) - v0;
		const Vec3 h = tinybvh_cross( state.D, e2 );
		const Float a = tinybvh_dot( e1, h );
		if (a == Float( 0 )) return false;
		const Float f = Float( 1 ) / a;
		const Vec3 s = state.O - v0;
		const Float u = f * tinybvh_dot( s, h );
		if (!(u >= Float( 0 ) && u <= Float( 1 ))) return false;
		const Vec3 q = tinybvh_cross( s, e1 );
		const Float v = f * tinybvh_dot( state.D, q );
		if (!(v >= Float( 0 ) && u + v <= Float( 1 ))) return false;
		const Float t = f * tinybvh_dot( e2, q );
		if (!(t >= m_tmin && t < m_tmax)) return false;
		tOut = t;
		uOut = u;
		vOut = v;
		return true;
	}

	void PushChild( const BVHNode* child, const Float tenter, BLASState* state )
	{
		if (tenter == Far() || tenter > m_tmax) return;
		m_frontier.push_back( FrontierItem{ child, tenter, state } );
		std::push_heap( m_frontier.begin(), m_frontier.end(), FrontierCmp() );
	}

	void Expand( const FrontierItem& item )
	{
		m_stats.nodeVisits++;
		const BVHNode* node = item.node;
		if (!node->isLeaf())
		{
			if (item.state == nullptr)
			{
				// TLAS interior node: children are tested in world space.
				const BVHNode* c1 = &m_TLAS.bvhNode[node->leftFirst];
				const BVHNode* c2 = &m_TLAS.bvhNode[node->leftFirst + 1];
				PushChild( c1, BoxEntry( c1, m_O, m_rD ), nullptr );
				PushChild( c2, BoxEntry( c2, m_O, m_rD ), nullptr );
			}
			else
			{
				// BLAS interior node: children are tested in the instance frame.
				BLASState* state = item.state;
				const BVH& blas = *state->blas;
				const BVHNode* c1 = &blas.bvhNode[node->leftFirst];
				const BVHNode* c2 = &blas.bvhNode[node->leftFirst + 1];
				PushChild( c1, BoxEntry( c1, state->O, state->rD ), state );
				PushChild( c2, BoxEntry( c2, state->O, state->rD ), state );
			}
			return;
		}
		if (item.state == nullptr) ExpandTLASLeaf( node );
		else ExpandBLASLeaf( item.state, node );
	}

	void ExpandTLASLeaf( const BVHNode* node )
	{
		for (Index slot = 0; slot < node->triCount; slot++)
		{
			const Index instId = m_TLAS.primIdx[node->leftFirst + slot];
			const Instance& inst = m_TLAS.instList[instId];
			if (!(inst.mask & m_mask)) continue; // bitwise mask rule, as in TLAS traversal
			const BVH* blas = (const BVH*)m_TLAS.blasList[inst.blasIdx];
			// Only binary LAYOUT_BVH trees are in scope; empty BLAS yield nothing.
			if (blas->layout != LAYOUT_BVH || blas->bvhNode == 0 || blas->triCount == 0) continue;
			m_stats.instanceEntries++;
			auto found = m_stateByInst.find( instId );
			BLASState* state;
			if (found == m_stateByInst.end())
			{
				m_states.emplace_back();
				state = &m_states.back();
				state->blas = blas;
				state->instId = instId;
				state->O = tinybvh_transform_point( m_O, inst.invTransform );
				state->D = tinybvh_transform_vector( m_D, inst.invTransform );
				state->rD = tinybvh_rcp( state->D );
				m_stateByInst.emplace( instId, state );
				const Float entry = BoxEntry( &blas->bvhNode[0], state->O, state->rD );
				// Affine transforms preserve the ray parameter, so the world
				// [tmin, tmax) interval applies unchanged in the local frame.
				PushChild( &blas->bvhNode[0], entry, state );
			}
		}
	}

	void ExpandBLASLeaf( BLASState* state, const BVHNode* node )
	{
		const BVH& blas = *state->blas;
		for (Index slot = 0; slot < node->triCount; slot++)
		{
			const Index prim = blas.primIdx[node->leftFirst + slot];
			// A primitive split across several leaves (spatial splits) is
			// intersected at most once within this instance.
			if (!state->testedPrims.insert( prim ).second) continue;
			m_stats.triangleTests++;
			Float t, u, v;
			if (!TriHit( *state, blas, prim, t, u, v )) continue;
			m_candidates.push_back( Candidate{ t, u, v, prim, state } );
			std::push_heap( m_candidates.begin(), m_candidates.end(), CandidateCmp() );
		}
	}

	const BVH& m_TLAS;
	const Vec3 m_O, m_D, m_rD; // world ray; never modified
	const uint32_t m_mask;
	const Float m_tmin, m_tmax;
	bool m_done = false;
	OrderedQueryStats m_stats;
	std::vector<FrontierItem> m_frontier;
	std::deque<BLASState> m_states; // references stay valid across emplace_back
	std::unordered_map<Index, BLASState*> m_stateByInst;
	std::vector<Candidate> m_candidates;
};

} // namespace ordered_detail

using OrderedHitQuery = ordered_detail::OrderedQueryImpl<float, uint32_t>;
using OrderedHitQueryEx = ordered_detail::OrderedQueryImpl<double, uint64_t>;

} // namespace tinybvh

#endif // TINY_BVH_ORDERED_H_
