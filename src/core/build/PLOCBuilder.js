import { getBounds } from './computeBoundsUtils.js';
import { BYTES_PER_NODE, UINT32_PER_NODE, IS_LEAFNODE_FLAG } from '../Constants.js';

const MAX_POINTER = Math.pow( 2, 32 );

// number of active positions inspected on either side of a cluster when searching
// for its nearest neighbor in each aggregation round
export const PLOC_NEIGHBOR_WINDOW = 16;

// scratch targets for "getBounds"
const _nodeBounds = /* @__PURE__ */ new Float32Array( 6 );
const _centroidBounds = /* @__PURE__ */ new Float64Array( 6 );

// spreads the 10 bits of "v" into every third slot of a 30 bit value
function expandMortonBits( v ) {

	v = ( v | ( v << 16 ) ) & 0x030000ff;
	v = ( v | ( v << 8 ) ) & 0x0300f00f;
	v = ( v | ( v << 4 ) ) & 0x030c30c3;
	v = ( v | ( v << 2 ) ) & 0x09249249;
	return v;

}

// quantizes "value" into 10 bits over the range [ min, min + span ]. A zero span maps
// to 0, otherwise the normalized coordinate is scaled by 1024 and clamped to 1023.
function quantizeAxis( value, min, span ) {

	if ( span === 0 ) {

		return 0;

	}

	const x = ( value - min ) / span;
	return Math.min( 1023, Math.floor( 1024 * x ) );

}

// 30 bit morton code of the given center relative to the centroid range in
// "centroidBounds" (min / max format). Non finite values degenerate to 0.
function computeMortonCode( cx, cy, cz, centroidBounds ) {

	const qx = quantizeAxis( cx, centroidBounds[ 0 ], centroidBounds[ 3 ] - centroidBounds[ 0 ] );
	const qy = quantizeAxis( cy, centroidBounds[ 1 ], centroidBounds[ 4 ] - centroidBounds[ 1 ] );
	const qz = quantizeAxis( cz, centroidBounds[ 2 ], centroidBounds[ 5 ] - centroidBounds[ 2 ] );
	return expandMortonBits( qx ) | ( expandMortonBits( qy ) << 1 ) | ( expandMortonBits( qz ) << 2 );

}

// surface area of the union of the two min / max boxes stored at "i6" and "j6" in
// "bounds". All arithmetic is plain JavaScript Number math so every caller - main
// thread or worker - produces bit identical results.
function unionSurfaceArea( bounds, i6, j6 ) {

	const minX = bounds[ i6 + 0 ] < bounds[ j6 + 0 ] ? bounds[ i6 + 0 ] : bounds[ j6 + 0 ];
	const minY = bounds[ i6 + 1 ] < bounds[ j6 + 1 ] ? bounds[ i6 + 1 ] : bounds[ j6 + 1 ];
	const minZ = bounds[ i6 + 2 ] < bounds[ j6 + 2 ] ? bounds[ i6 + 2 ] : bounds[ j6 + 2 ];
	const maxX = bounds[ i6 + 3 ] > bounds[ j6 + 3 ] ? bounds[ i6 + 3 ] : bounds[ j6 + 3 ];
	const maxY = bounds[ i6 + 4 ] > bounds[ j6 + 4 ] ? bounds[ i6 + 4 ] : bounds[ j6 + 4 ];
	const maxZ = bounds[ i6 + 5 ] > bounds[ j6 + 5 ] ? bounds[ i6 + 5 ] : bounds[ j6 + 5 ];

	const d0 = maxX - minX;
	const d1 = maxY - minY;
	const d2 = maxZ - minZ;
	return 2 * ( d0 * d1 + d1 * d2 + d2 * d0 );

}

/**
 * Creates the statistics object reported for a PLOC build. "rounds" holds the number
 * of active clusters at the start of every aggregation round with the final count of
 * 1 appended, "roundPairs" the merged position pairs of each round, and "threads" the
 * per thread work counters which are accumulated where the work is performed.
 */
export function createPLOCStats() {

	return {
		rounds: [],
		roundPairs: [],
		candidateEvaluations: 0,
		mergedPairs: 0,
		threads: [ { candidateEvaluations: 0, mergedPairs: 0 } ],
	};

}

/**
 * Initializes the PLOC clustering state for the primitives in [ offset, offset + count ).
 * Every primitive becomes one leaf cluster; the initial active order is the stable sort
 * of the primitives by the morton code of their bounds centers with ties broken by the
 * original face order. The morton codes are only used to derive this initial order.
 *
 * Cluster bounds are stored as float64 min / max values derived from the float32
 * center / half extent primitive bounds so later unions are exact min / max operations.
 *
 * @param {Float32Array} primitiveBounds - center / half extent bounds, 6 values per primitive
 * @param {number} offset - first primitive of the root range
 * @param {number} count - number of primitives in the root range
 * @returns {Object} the clustering state
 */
export function preparePLOCState( primitiveBounds, offset, count ) {

	const boundsOffset = primitiveBounds.offset || 0;
	const leafCount = Math.ceil( count );
	if ( ! ( leafCount >= 1 ) ) {

		throw new Error( 'PLOCBuilder: Cannot cluster an empty primitive range.' );

	}

	// centroid range of the whole root range used to normalize the centers
	getBounds( primitiveBounds, offset, count, _nodeBounds, _centroidBounds );

	const totalClusters = 2 * leafCount - 1;
	const bounds = new Float64Array( totalClusters * 6 );
	const minFace = new Int32Array( totalClusters );
	const primCount = new Int32Array( totalClusters );
	const left = new Int32Array( totalClusters );
	const right = new Int32Array( totalClusters );
	const leafIndex = new Float64Array( leafCount );
	const active = new Int32Array( leafCount );

	// morton code per primitive, keyed by local index so the sort can look them up
	const codes = new Uint32Array( leafCount );
	for ( let local = 0; local < leafCount; local ++ ) {

		const b6 = ( offset + local - boundsOffset ) * 6;
		codes[ local ] = computeMortonCode(
			primitiveBounds[ b6 + 0 ],
			primitiveBounds[ b6 + 2 ],
			primitiveBounds[ b6 + 4 ],
			_centroidBounds,
		);

	}

	// stable order of the primitives by morton code, then by original face order
	const order = new Int32Array( leafCount );
	for ( let i = 0; i < leafCount; i ++ ) {

		order[ i ] = i;

	}

	order.sort( ( a, b ) => ( codes[ a ] - codes[ b ] ) || ( a - b ) );

	// initialize the leaf clusters in morton order
	for ( let k = 0; k < leafCount; k ++ ) {

		const local = order[ k ];
		const pos = offset + local;
		const b6 = ( pos - boundsOffset ) * 6;
		const b6k = k * 6;
		bounds[ b6k + 0 ] = primitiveBounds[ b6 + 0 ] - primitiveBounds[ b6 + 1 ];
		bounds[ b6k + 1 ] = primitiveBounds[ b6 + 2 ] - primitiveBounds[ b6 + 3 ];
		bounds[ b6k + 2 ] = primitiveBounds[ b6 + 4 ] - primitiveBounds[ b6 + 5 ];
		bounds[ b6k + 3 ] = primitiveBounds[ b6 + 0 ] + primitiveBounds[ b6 + 1 ];
		bounds[ b6k + 4 ] = primitiveBounds[ b6 + 2 ] + primitiveBounds[ b6 + 3 ];
		bounds[ b6k + 5 ] = primitiveBounds[ b6 + 4 ] + primitiveBounds[ b6 + 5 ];
		minFace[ k ] = pos;
		primCount[ k ] = 1;
		left[ k ] = - 1;
		right[ k ] = - 1;
		leafIndex[ k ] = pos;
		active[ k ] = k;

	}

	return {
		count: leafCount,
		bounds,
		minFace,
		primCount,
		left,
		right,
		leafIndex,
		active,
		activeCount: leafCount,
		nextClusterId: leafCount,
		rootId: leafCount === 1 ? 0 : - 1,
	};

}

/**
 * Copies the bounds and minimum face indices of the currently active clusters into
 * compact arrays in active order. This is the frozen snapshot a round operates on -
 * new clusters are never visible until the round is fully merged and published.
 *
 * @param {Object} state - clustering state from "preparePLOCState"
 * @returns {{ bounds: Float64Array, minFace: Int32Array }}
 */
export function compactActiveClusters( state ) {

	const m = state.activeCount;
	const bounds = new Float64Array( m * 6 );
	const minFace = new Int32Array( m );
	const active = state.active;
	const clusterBounds = state.bounds;
	const clusterMinFace = state.minFace;
	for ( let i = 0; i < m; i ++ ) {

		const id = active[ i ];
		const b6 = id * 6;
		const i6 = i * 6;
		for ( let j = 0; j < 6; j ++ ) {

			bounds[ i6 + j ] = clusterBounds[ b6 + j ];

		}

		minFace[ i ] = clusterMinFace[ id ];

	}

	return { bounds, minFace };

}

/**
 * Computes the nearest neighbor of every active cluster in [ start, end ). Each cluster
 * inspects up to "PLOC_NEIGHBOR_WINDOW" active positions on either side and picks the
 * candidate whose union bounds have the smallest surface area; ties are broken by the
 * smallest original face index contained in the neighbor cluster, which is unique and
 * therefore a total order. The arrays may be a windowed slice of the full active data -
 * "windowOffset" is the active position of element 0 in the slices.
 *
 * This function is pure and is what both the synchronous builder and the worker
 * threads execute, so candidate evaluation counts are accumulated at the call site.
 *
 * @returns {number} the number of union area evaluations performed
 */
export function proposeNeighbors( activeBounds, activeMinFace, windowOffset, totalCount, start, end, outBest ) {

	let evaluations = 0;
	for ( let p = start; p < end; p ++ ) {

		const p6 = ( p - windowOffset ) * 6;
		let bestPos = - 1;
		let bestArea = Infinity;
		let bestFace = Infinity;
		const lo = Math.max( 0, p - PLOC_NEIGHBOR_WINDOW );
		const hi = Math.min( totalCount - 1, p + PLOC_NEIGHBOR_WINDOW );
		for ( let q = lo; q <= hi; q ++ ) {

			if ( q === p ) {

				continue;

			}

			const lq = q - windowOffset;
			const area = unionSurfaceArea( activeBounds, p6, lq * 6 );
			evaluations ++;

			const face = activeMinFace[ lq ];
			if ( area < bestArea || ( area === bestArea && face < bestFace ) ) {

				bestArea = area;
				bestFace = face;
				bestPos = q;

			}

		}

		outBest[ p - start ] = bestPos;

	}

	return evaluations;

}

/**
 * Determines the pairs to merge in a round from the proposed nearest neighbors. Every
 * mutually nearest pair ( i -> j and j -> i ) merges simultaneously; such pairs are
 * always disjoint. The result is ordered by the earlier position of each pair.
 *
 * With degenerate (non finite) bounds it is possible that no mutual pair exists. In
 * that case adjacent clusters are paired so the round still reduces the cluster count
 * and the build is guaranteed to terminate.
 *
 * @param {Int32Array} best - proposed neighbor per active position
 * @param {number} count - number of active clusters
 * @returns {Array<Array<number>>} pairs of active positions [ earlier, later ]
 */
export function computeRoundPairs( best, count ) {

	const pairs = [];
	for ( let p = 0; p < count; p ++ ) {

		const q = best[ p ];
		if ( q > p && q < count && best[ q ] === p ) {

			pairs.push( [ p, q ] );

		}

	}

	if ( pairs.length === 0 && count >= 2 ) {

		for ( let p = 0; p + 1 < count; p += 2 ) {

			pairs.push( [ p, p + 1 ] );

		}

	}

	return pairs;

}

/**
 * Gathers the bounds and minimum face indices of the two children of every pair into
 * flat arrays so the parent bounds can be computed in bulk (possibly by workers).
 *
 * @param {Float64Array} activeBounds - active cluster bounds in active order
 * @param {Int32Array} activeMinFace - active minimum face indices in active order
 * @param {Array<Array<number>>} pairs - pairs from "computeRoundPairs"
 */
export function gatherPairData( activeBounds, activeMinFace, pairs ) {

	const pairCount = pairs.length;
	const leftBounds = new Float64Array( pairCount * 6 );
	const rightBounds = new Float64Array( pairCount * 6 );
	const leftMinFace = new Int32Array( pairCount );
	const rightMinFace = new Int32Array( pairCount );
	for ( let k = 0; k < pairCount; k ++ ) {

		const pair = pairs[ k ];
		const l6 = pair[ 0 ] * 6;
		const r6 = pair[ 1 ] * 6;
		const k6 = k * 6;
		for ( let j = 0; j < 6; j ++ ) {

			leftBounds[ k6 + j ] = activeBounds[ l6 + j ];
			rightBounds[ k6 + j ] = activeBounds[ r6 + j ];

		}

		leftMinFace[ k ] = activeMinFace[ pair[ 0 ] ];
		rightMinFace[ k ] = activeMinFace[ pair[ 1 ] ];

	}

	return { leftBounds, rightBounds, leftMinFace, rightMinFace };

}

/**
 * Computes the parent cluster bounds and minimum face indices for a set of pairs. The
 * union is an exact component wise min / max so the result does not depend on which
 * thread performs the reduction or in what order the pairs are processed.
 *
 * This function is pure and is what both the synchronous builder and the worker
 * threads execute for the merge phase.
 */
export function mergeClusterBounds( leftBounds, rightBounds, leftMinFace, rightMinFace, count, outBounds, outMinFace ) {

	for ( let k = 0; k < count; k ++ ) {

		const k6 = k * 6;
		for ( let j = 0; j < 6; j ++ ) {

			const l = leftBounds[ k6 + j ];
			const r = rightBounds[ k6 + j ];
			outBounds[ k6 + j ] = j < 3 ? ( l < r ? l : r ) : ( l > r ? l : r );

		}

		const lf = leftMinFace[ k ];
		const rf = rightMinFace[ k ];
		outMinFace[ k ] = lf < rf ? lf : rf;

	}

}

/**
 * Publishes the merged pairs into the clustering state and derives the active order
 * for the next round: the parent cluster takes the earlier position of its pair, the
 * later position is removed, and unmerged clusters keep their relative order. Children
 * are stored ordered by their pre-merge positions.
 *
 * @param {Object} state - clustering state from "preparePLOCState"
 * @param {Array<Array<number>>} pairs - pairs from "computeRoundPairs"
 * @param {Float64Array} parentBounds - merged bounds, 6 values per pair
 * @param {Int32Array} parentMinFace - merged minimum face indices, 1 per pair
 */
export function applyMergeResults( state, pairs, parentBounds, parentMinFace ) {

	const m = state.activeCount;
	const active = state.active;
	const left = state.left;
	const right = state.right;
	const bounds = state.bounds;
	const minFace = state.minFace;
	const primCount = state.primCount;

	const parentAt = new Int32Array( m ).fill( - 1 );
	const removed = new Uint8Array( m );
	let nextClusterId = state.nextClusterId;
	for ( let k = 0, l = pairs.length; k < l; k ++ ) {

		const pair = pairs[ k ];
		const id = nextClusterId ++;
		const earlier = active[ pair[ 0 ] ];
		const later = active[ pair[ 1 ] ];

		left[ id ] = earlier;
		right[ id ] = later;

		const b6 = id * 6;
		const k6 = k * 6;
		for ( let j = 0; j < 6; j ++ ) {

			bounds[ b6 + j ] = parentBounds[ k6 + j ];

		}

		minFace[ id ] = parentMinFace[ k ];
		primCount[ id ] = primCount[ earlier ] + primCount[ later ];
		parentAt[ pair[ 0 ] ] = id;
		removed[ pair[ 1 ] ] = 1;

	}

	const nextActive = new Int32Array( m - pairs.length );
	let index = 0;
	for ( let p = 0; p < m; p ++ ) {

		if ( removed[ p ] ) {

			continue;

		}

		nextActive[ index ++ ] = parentAt[ p ] === - 1 ? active[ p ] : parentAt[ p ];

	}

	state.active = nextActive;
	state.activeCount = nextActive.length;
	state.nextClusterId = nextClusterId;

}

/**
 * Runs the full aggregation synchronously and returns the build statistics. This is
 * the serial counterpart of the worker driven build and performs the exact same
 * operations in the same order, so both produce identical trees.
 *
 * @param {Object} state - clustering state from "preparePLOCState"
 * @param {Object} [stats] - statistics object to accumulate into
 * @returns {Object} stats
 */
export function runPLOCClusteringSync( state, stats = createPLOCStats() ) {

	const threadStats = stats.threads[ 0 ];
	while ( state.activeCount > 1 ) {

		stats.rounds.push( state.activeCount );
		const m = state.activeCount;
		const activeData = compactActiveClusters( state );
		const bounds = activeData.bounds;
		const minFace = activeData.minFace;

		const best = new Int32Array( m );
		const evaluations = proposeNeighbors( bounds, minFace, 0, m, 0, m, best );
		stats.candidateEvaluations += evaluations;
		threadStats.candidateEvaluations += evaluations;

		const pairs = computeRoundPairs( best, m );
		stats.roundPairs.push( pairs );

		const pairData = gatherPairData( bounds, minFace, pairs );
		const pairCount = pairs.length;
		const parentBounds = new Float64Array( pairCount * 6 );
		const parentMinFace = new Int32Array( pairCount );
		mergeClusterBounds( pairData.leftBounds, pairData.rightBounds, pairData.leftMinFace, pairData.rightMinFace, pairCount, parentBounds, parentMinFace );
		stats.mergedPairs += pairCount;
		threadStats.mergedPairs += pairCount;

		applyMergeResults( state, pairs, parentBounds, parentMinFace );

	}

	stats.rounds.push( state.activeCount );
	state.rootId = state.active[ 0 ];
	return stats;

}

/**
 * Lays out the aggregated cluster hierarchy into the packed node format, depth first,
 * and permutes the partition buffer (the geometry index or the indirect buffer) so the
 * primitives of every leaf occupy a contiguous range. Subtrees with at most
 * "targetLeafSize" primitives - or any subtree at "maxDepth" - are folded into a
 * single leaf spanning their whole contiguous primitive range.
 *
 * @param {BVH} bvh - the BVH whose partition buffer is permuted
 * @param {Object} state - completed clustering state
 * @param {number} offset - first primitive of the root range
 * @param {number} count - number of primitives in the root range
 * @param {Object} options - build options (targetLeafSize, maxDepth, verbose, onProgress, ...)
 * @param {Object} loadRange - full range of all roots, for progress reporting
 * @returns {ArrayBufferLike} packed node buffer trimmed to the written nodes
 */
export function layoutPLOCTree( bvh, state, offset, count, options, loadRange ) {

	const {
		maxDepth,
		verbose,
		targetLeafSize,
		_strictLeafSize = Infinity,
		onProgress,
		useSharedArrayBuffer = false,
	} = options;

	const partitionBuffer = bvh.primitiveBuffer;
	const partitionStride = bvh.primitiveBufferStride;

	// a binary tree with at least one primitive per leaf has at most 2n - 1 nodes
	const BufferConstructor = useSharedArrayBuffer ? SharedArrayBuffer : ArrayBuffer;
	const buffer = new BufferConstructor( BYTES_PER_NODE * Math.max( 1, 2 * Math.ceil( count ) - 1 ) );
	const float32Array = new Float32Array( buffer );
	const uint32Array = new Uint32Array( buffer );
	const uint16Array = new Uint16Array( buffer );

	const bounds = state.bounds;
	const left = state.left;
	const right = state.right;
	const primCount = state.primCount;
	const leafIndex = state.leafIndex;
	const faceOrder = new Float64Array( state.count );
	let nodeCount = 0;
	let faceSlot = 0;
	let reachedMaxDepth = false;

	writeCluster( state.rootId, 0 );
	permutePartitionBuffer();
	return buffer.slice( 0, nodeCount * BYTES_PER_NODE );

	// writes the node for "id" and either folds the subtree into a leaf or recurses
	function writeCluster( id, depth ) {

		const nodeIndex = nodeCount ++;
		const node32 = nodeIndex * UINT32_PER_NODE;
		const node16 = node32 * 2;
		const b6 = id * 6;
		for ( let i = 0; i < 6; i ++ ) {

			float32Array[ node32 + i ] = bounds[ b6 + i ];

		}

		if ( ! reachedMaxDepth && depth >= maxDepth ) {

			reachedMaxDepth = true;
			if ( verbose ) {

				console.warn( `BVH: Max depth of ${ maxDepth } reached when generating BVH. Consider increasing maxDepth.` );

			}

		}

		const primitiveCount = primCount[ id ];
		const isLeafCluster = left[ id ] === - 1;

		// fold the whole subtree into a contiguous leaf range when the truncation
		// limits are hit
		if ( ( primitiveCount <= targetLeafSize && primitiveCount <= _strictLeafSize ) || depth >= maxDepth || isLeafCluster ) {

			const leafOffset = offset + faceSlot;
			uint32Array[ node32 + 6 ] = leafOffset;
			uint16Array[ node16 + 14 ] = primitiveCount;
			uint16Array[ node16 + 15 ] = IS_LEAFNODE_FLAG;
			emitLeaves( id );
			if ( onProgress ) {

				onProgress( ( leafOffset + primitiveCount - loadRange.offset ) / loadRange.count );

			}

			return;

		}

		// PLOC merges are not axis splits, so the split axis is always 0
		uint32Array[ node32 + 7 ] = 0;
		writeCluster( left[ id ], depth + 1 );

		// the right child follows the left subtree, stored as an offset relative to this node
		const relativeRightIndex = nodeCount - nodeIndex;
		if ( relativeRightIndex > MAX_POINTER ) {

			throw new Error( 'MeshBVH: Cannot store relative child node offset greater than 32 bits.' );

		}

		uint32Array[ node32 + 6 ] = relativeRightIndex;
		writeCluster( right[ id ], depth + 1 );

	}

	// emits the leaf primitives of the subtree rooted at "id" in depth first order
	function emitLeaves( id ) {

		const stack = [ id ];
		while ( stack.length > 0 ) {

			const current = stack.pop();
			if ( left[ current ] === - 1 ) {

				faceOrder[ faceSlot ++ ] = leafIndex[ current ];

			} else {

				stack.push( right[ current ] );
				stack.push( left[ current ] );

			}

		}

	}

	// scatters the primitives of the root range into the depth first slot order
	function permutePartitionBuffer() {

		const stride = partitionStride;
		const start = offset * stride;
		const end = ( offset + count ) * stride;
		const copy = partitionBuffer.slice( start, end );
		for ( let s = 0; s < faceSlot; s ++ ) {

			const src = ( faceOrder[ s ] - offset ) * stride;
			const dst = s * stride;
			for ( let k = 0; k < stride; k ++ ) {

				partitionBuffer[ start + dst + k ] = copy[ src + k ];

			}

		}

	}

}

// writes a single empty leaf node, matching the buffer the default builder produces
// for an empty range so empty geometry stays queryable
function buildEmptyLeaf( offset, options ) {

	const BufferConstructor = options.useSharedArrayBuffer ? SharedArrayBuffer : ArrayBuffer;
	const buffer = new BufferConstructor( BYTES_PER_NODE );
	const float32Array = new Float32Array( buffer );
	const uint32Array = new Uint32Array( buffer );
	const uint16Array = new Uint16Array( buffer );

	// bounds of zero primitives are all infinities, as produced by "getBounds"
	getBounds( new Float32Array( 0 ), 0, 0, _nodeBounds, _centroidBounds );
	float32Array.set( _nodeBounds, 0 );
	uint32Array[ 6 ] = offset;
	uint16Array[ 14 ] = 0;
	uint16Array[ 15 ] = IS_LEAFNODE_FLAG;
	return buffer;

}

/**
 * Builds a packed BVH root for the given primitive range using bottom-up PLOC
 * aggregation. Mirrors the signature and output format of "buildTree" so the result
 * can be used directly as a BVH root.
 *
 * @param {BVH} bvh - the BVH being built
 * @param {Float32Array} primitiveBounds - center / half extent bounds of all primitives
 * @param {number} offset - first primitive of the root range
 * @param {number} count - number of primitives in the root range
 * @param {Object} options - build options
 * @param {Object} loadRange - full range of all roots, for progress reporting
 * @returns {ArrayBufferLike} packed node buffer
 */
export function buildTreePLOC( bvh, primitiveBounds, offset, count, options, loadRange ) {

	if ( count < 1 ) {

		return buildEmptyLeaf( offset, options );

	}

	const state = preparePLOCState( primitiveBounds, offset, count );
	runPLOCClusteringSync( state );
	return layoutPLOCTree( bvh, state, offset, count, options, loadRange );

}
