import {
	BYTES_PER_NODE,
	UINT32_PER_NODE,
	IS_LEAFNODE_FLAG,
	PLOC,
} from '../Constants.js';
import { getRootPrimitiveRanges } from './geometryUtils.js';

export const PLOC_NEIGHBOR_RADIUS = 16;
export const PLOC_MORTON_BITS = 10;
const PLOC_MORTON_MAX = ( 1 << PLOC_MORTON_BITS ) - 1;

const _tempBounds = new Float64Array( 6 );

/**
 * Expands the low 10 bits of a coordinate so they occupy every third bit.
 * @param {number} value
 * @returns {number}
 */
export function expandMorton10( value ) {

	let x = value & PLOC_MORTON_MAX;
	x = ( x | ( x << 16 ) ) & 0x30000ff;
	x = ( x | ( x << 8 ) ) & 0x300f00f;
	x = ( x | ( x << 4 ) ) & 0x30c30c3;
	x = ( x | ( x << 2 ) ) & 0x9249249;
	return x;

}

/**
 * Returns the 30 bit xyz Morton code for three 10 bit coordinates.
 * @param {number} x
 * @param {number} y
 * @param {number} z
 * @returns {number}
 */
export function morton3D10( x, y, z ) {

	return expandMorton10( x ) |
		( expandMorton10( y ) << 1 ) |
		( expandMorton10( z ) << 2 );

}

/**
 * Quantizes a centroid relative to the centroid bounds of one root range.
 * @param {number} center
 * @param {number} min
 * @param {number} span
 * @returns {number}
 */
export function quantizeCentroid10( center, min, span ) {

	if ( span === 0 ) {

		return 0;

	}

	const normalized = ( center - min ) / span;
	return Math.min( PLOC_MORTON_MAX, Math.floor( 1024 * normalized ) );

}

/**
 * Surface area of a min/max bounds array using the canonical PLOC operation order.
 * @param {ArrayLike<number>} bounds
 * @returns {number}
 */
export function surfaceArea6( bounds ) {

	const dx = bounds[ 3 ] - bounds[ 0 ];
	const dy = bounds[ 4 ] - bounds[ 1 ];
	const dz = bounds[ 5 ] - bounds[ 2 ];
	return 2 * ( dx * dy + dy * dz + dz * dx );

}

/**
 * Writes the union of two min/max bounds into `target`.
 * @param {ArrayLike<number>} a
 * @param {ArrayLike<number>} b
 * @param {Float64Array|number[]} target
 * @returns {Float64Array|number[]}
 */
export function unionBounds6( a, b, target = new Float64Array( 6 ) ) {

	target[ 0 ] = a[ 0 ] < b[ 0 ] ? a[ 0 ] : b[ 0 ];
	target[ 1 ] = a[ 1 ] < b[ 1 ] ? a[ 1 ] : b[ 1 ];
	target[ 2 ] = a[ 2 ] < b[ 2 ] ? a[ 2 ] : b[ 2 ];
	target[ 3 ] = a[ 3 ] > b[ 3 ] ? a[ 3 ] : b[ 3 ];
	target[ 4 ] = a[ 4 ] > b[ 4 ] ? a[ 4 ] : b[ 4 ];
	target[ 5 ] = a[ 5 ] > b[ 5 ] ? a[ 5 ] : b[ 5 ];
	return target;

}

/**
 * Computes the union surface area without allocating merged bounds. The arithmetic
 * order is intentionally identical to unionBounds6() followed by surfaceArea6().
 * @param {ArrayLike<number>} a
 * @param {ArrayLike<number>} b
 * @returns {number}
 */
export function unionSurfaceArea6( a, b ) {

	const minX = a[ 0 ] < b[ 0 ] ? a[ 0 ] : b[ 0 ];
	const minY = a[ 1 ] < b[ 1 ] ? a[ 1 ] : b[ 1 ];
	const minZ = a[ 2 ] < b[ 2 ] ? a[ 2 ] : b[ 2 ];
	const maxX = a[ 3 ] > b[ 3 ] ? a[ 3 ] : b[ 3 ];
	const maxY = a[ 4 ] > b[ 4 ] ? a[ 4 ] : b[ 4 ];
	const maxZ = a[ 5 ] > b[ 5 ] ? a[ 5 ] : b[ 5 ];
	const dx = maxX - minX;
	const dy = maxY - minY;
	const dz = maxZ - minZ;
	return 2 * ( dx * dy + dy * dz + dz * dx );

}

function snapshotCluster( cluster ) {

	return {
		id: cluster.id,
		minOriginal: cluster.minOriginal,
		count: cluster.count,
		bounds: Array.from( cluster.bounds ),
	};

}

/**
 * Evaluates the best local-window neighbor for one active cluster position.
 * This is the unit of candidate work distributed to Node workers.
 * @param {Array<object>} clusters Frozen active cluster order.
 * @param {number} index Position whose candidates are evaluated.
 * @param {number} [radius]
 * @returns {{index:number,best:number,area:number,evaluations:number}}
 */
export function evaluateCandidateAt( clusters, index, radius = PLOC_NEIGHBOR_RADIUS ) {

	const cluster = clusters[ index ];
	const start = Math.max( 0, index - radius );
	const end = Math.min( clusters.length - 1, index + radius );

	let best = - 1;
	let bestArea = Infinity;
	let bestMinOriginal = Infinity;
	let evaluations = 0;

	for ( let j = start; j <= end; j ++ ) {

		if ( j === index ) {

			continue;

		}

		evaluations ++;
		const candidate = clusters[ j ];
		const area = unionSurfaceArea6( cluster.bounds, candidate.bounds );
		const candidateMinOriginal = candidate.minOriginal;

		if (
			area < bestArea ||
			( area === bestArea && candidateMinOriginal < bestMinOriginal ) ||
			( area === bestArea && candidateMinOriginal === bestMinOriginal && j < best )
		) {

			bestArea = area;
			bestMinOriginal = candidateMinOriginal;
			best = j;

		}

	}

	return { index, best, area: bestArea, evaluations };

}

/**
 * Evaluates a batch of candidate positions. Used directly by worker threads so
 * candidateEvaluations is incremented at the site where the arithmetic runs.
 * @param {object} params
 * @returns {{proposals:Array<object>,candidateEvaluations:number}}
 */
export function evaluateCandidateBatch( { clusters, indices, radius = PLOC_NEIGHBOR_RADIUS } ) {

	const proposals = [];
	let candidateEvaluations = 0;
	for ( const index of indices ) {

		const proposal = evaluateCandidateAt( clusters, index, radius );
		candidateEvaluations += proposal.evaluations;
		proposals.push( proposal );

	}

	return { proposals, candidateEvaluations };

}

/**
 * Computes one real parent bounds merge in a worker.
 * @param {object} params
 * @returns {object}
 */
export function mergeClusterPair( { pairId, a, b } ) {

	const bounds = Array.from( unionBounds6( a.bounds, b.bounds, new Float64Array( 6 ) ) );
	return {
		pairId,
		bounds,
		minOriginal: Math.min( a.minOriginal, b.minOriginal ),
		count: a.count + b.count,
	};

}

/**
 * Merges a batch of pairs in a worker thread.
 * @param {object} params
 * @returns {{merges:Array<object>,mergedPairs:number}}
 */
export function mergeClusterPairBatch( { pairs } ) {

	const merges = [];
	for ( const pair of pairs ) {

		merges.push( mergeClusterPair( pair ) );

	}

	return { merges, mergedPairs: pairs.length };

}

function createLeafCluster( id, faceLocal, original, bounds ) {

	return {
		id,
		faceLocal,
		minOriginal: original,
		count: 1,
		bounds: Float64Array.from( bounds ),
		left: null,
		right: null,
	};

}

function createInitialPLOCClusters( { original, bounds, faceCount } ) {

	const centersX = new Float64Array( faceCount );
	const centersY = new Float64Array( faceCount );
	const centersZ = new Float64Array( faceCount );
	let minX = Infinity, minY = Infinity, minZ = Infinity;
	let maxX = - Infinity, maxY = - Infinity, maxZ = - Infinity;

	for ( let i = 0; i < faceCount; i ++ ) {

		const base = i * 6;
		const cx = ( bounds[ base + 0 ] + bounds[ base + 3 ] ) / 2;
		const cy = ( bounds[ base + 1 ] + bounds[ base + 4 ] ) / 2;
		const cz = ( bounds[ base + 2 ] + bounds[ base + 5 ] ) / 2;
		centersX[ i ] = cx;
		centersY[ i ] = cy;
		centersZ[ i ] = cz;
		if ( cx < minX ) minX = cx;
		if ( cy < minY ) minY = cy;
		if ( cz < minZ ) minZ = cz;
		if ( cx > maxX ) maxX = cx;
		if ( cy > maxY ) maxY = cy;
		if ( cz > maxZ ) maxZ = cz;

	}

	const spanX = maxX - minX;
	const spanY = maxY - minY;
	const spanZ = maxZ - minZ;
	const codes = new Uint32Array( faceCount );
	for ( let i = 0; i < faceCount; i ++ ) {

		const x = quantizeCentroid10( centersX[ i ], minX, spanX );
		const y = quantizeCentroid10( centersY[ i ], minY, spanY );
		const z = quantizeCentroid10( centersZ[ i ], minZ, spanZ );
		codes[ i ] = morton3D10( x, y, z );

	}

	// Comparator includes the original face order, making equal Morton codes stable.
	const order = Array.from( { length: faceCount }, ( _, i ) => i );
	order.sort( ( a, b ) => {

		const codeDelta = codes[ a ] - codes[ b ];
		if ( codeDelta !== 0 ) return codeDelta;
		const originalDelta = original[ a ] - original[ b ];
		if ( originalDelta !== 0 ) return originalDelta;
		return a - b;

	} );

	const clusters = order.map( faceLocal => createLeafCluster(
		faceLocal,
		faceLocal,
		original[ faceLocal ],
		[
			bounds[ faceLocal * 6 + 0 ],
			bounds[ faceLocal * 6 + 1 ],
			bounds[ faceLocal * 6 + 2 ],
			bounds[ faceLocal * 6 + 3 ],
			bounds[ faceLocal * 6 + 4 ],
			bounds[ faceLocal * 6 + 5 ],
		],
	) );

	return { order, clusters };

}

/**
 * Builds the complete bottom-up PLOC hierarchy for one root range synchronously.
 * The returned round records contain the frozen active count and mutual pairs.
 * @param {object} rootFaces
 * @param {Array<number>|Uint32Array} rootFaces.original Original global face indices.
 * @param {Array<ArrayLike<number>>|Float64Array} rootFaces.bounds Face min/max bounds.
 * @param {number} [faceCount]
 * @returns {{root:object|null,rounds:Array<object>,order:number[]}}
 */
export function clusterPLOCSerial( { original, bounds, faceCount = original.length } ) {

	if ( faceCount === 0 ) {

		return { root: null, rounds: [], order: [] };

	}

	const { order, clusters: initialClusters } = createInitialPLOCClusters( { original, bounds, faceCount } );
	let nextId = faceCount;
	let clusters = initialClusters;

	const rounds = [];
	let roundIndex = 0;
	while ( clusters.length > 1 ) {

		const activeClusters = clusters.length;
		const proposals = clusters.map( ( _, i ) => evaluateCandidateAt( clusters, i ) );
		const best = proposals.map( proposal => proposal.best );

		/** @type {Array<{early:number,late:number,pairId:number}>} */
		const pairPositions = [];
		for ( let i = 0; i < clusters.length; i ++ ) {

			const j = best[ i ];
			if ( j > i && best[ j ] === i ) {

				pairPositions.push( { early: i, late: j, pairId: pairPositions.length } );

			}

		}

		const pairs = pairPositions.map( ( { early, late, pairId } ) => ( {
			pairId,
			a: snapshotCluster( clusters[ early ] ),
			b: snapshotCluster( clusters[ late ] ),
		} ) );
		const merges = mergeClusterPairBatch( { pairs } ).merges;
		const mergeById = new Map( merges.map( merge => [ merge.pairId, merge ] ) );

		const parentByEarly = new Map();
		const latePositions = new Set();
		const pairRecords = [];
		for ( const pairPosition of pairPositions ) {

			const { early, late, pairId } = pairPosition;
			const left = clusters[ early ];
			const right = clusters[ late ];
			const merged = mergeById.get( pairId );
			const parent = {
				id: nextId ++,
				faceLocal: - 1,
				minOriginal: merged.minOriginal,
				count: merged.count,
				bounds: Float64Array.from( merged.bounds ),
				left,
				right,
			};
			parentByEarly.set( early, parent );
			latePositions.add( late );
			pairRecords.push( {
				early,
				late,
				leftMinOriginal: left.minOriginal,
				rightMinOriginal: right.minOriginal,
				minOriginal: merged.minOriginal,
			} );

		}

		const nextClusters = [];
		for ( let i = 0; i < clusters.length; i ++ ) {

			if ( parentByEarly.has( i ) ) {

				nextClusters.push( parentByEarly.get( i ) );

			} else if ( ! latePositions.has( i ) ) {

				nextClusters.push( clusters[ i ] );

			}

		}

		rounds.push( {
			round: roundIndex ++,
			activeClusters,
			clusters: activeClusters,
			mutualPairs: pairRecords,
			mergedPairs: pairRecords.length,
			candidateEvaluations: proposals.reduce( ( sum, p ) => sum + p.evaluations, 0 ),
			nextActiveClusters: nextClusters.length,
		} );

		clusters = nextClusters;

	}

	return { root: clusters[ 0 ] || null, rounds, order };

}

/**
 * Parallel version of clusterPLOCSerial(). `executor` receives named batches and
 * is responsible for running them in worker threads. It must preserve input order.
 * @param {object} rootFaces
 * @param {(name:string,payload:object)=>Promise<object>} executor
 * @returns {Promise<{root:object|null,rounds:Array<object>,order:number[],stats:object}>}
 */
export async function clusterPLOCParallel( { original, bounds, faceCount = original.length }, executor ) {

	if ( faceCount === 0 ) {

		return { root: null, rounds: [], order: [], stats: { candidateEvaluations: 0, mergedPairs: 0 } };

	}

	const { order, clusters: initialClusters } = createInitialPLOCClusters( { original, bounds, faceCount } );

	let nextId = faceCount;
	let clusters = initialClusters;

	const rounds = [];
	const stats = { candidateEvaluations: 0, mergedPairs: 0 };
	let roundIndex = 0;
	while ( clusters.length > 1 ) {

		const activeClusters = clusters.length;
		const snapshot = clusters.map( snapshotCluster );
		const indices = Array.from( { length: activeClusters }, ( _, i ) => i );
		const candidateResult = await executor( 'candidates', { clusters: snapshot, indices } );
		stats.candidateEvaluations += candidateResult.candidateEvaluations;

		const proposals = candidateResult.proposals.slice().sort( ( a, b ) => a.index - b.index );
		const best = new Array( activeClusters ).fill( - 1 );
		for ( const proposal of proposals ) {

			best[ proposal.index ] = proposal.best;

		}

		const pairPositions = [];
		for ( let i = 0; i < activeClusters; i ++ ) {

			const j = best[ i ];
			if ( j > i && best[ j ] === i ) {

				pairPositions.push( { early: i, late: j, pairId: pairPositions.length } );

			}

		}

		const pairPayload = pairPositions.map( ( { early, late, pairId } ) => ( {
			pairId,
			a: snapshot[ early ],
			b: snapshot[ late ],
		} ) );

		// The executor may split this batch across threads. Every returned merge is
		// real parent bounds arithmetic performed outside this orchestration thread.
		const mergeResult = pairPayload.length ?
			await executor( 'merges', { pairs: pairPayload } ) :
			{ merges: [], mergedPairs: 0 };
		stats.mergedPairs += mergeResult.mergedPairs;
		const mergeById = new Map( mergeResult.merges.map( merge => [ merge.pairId, merge ] ) );

		const parentByEarly = new Map();
		const latePositions = new Set();
		const pairRecords = [];
		for ( const pairPosition of pairPositions ) {

			const { early, late, pairId } = pairPosition;
			const left = clusters[ early ];
			const right = clusters[ late ];
			const merged = mergeById.get( pairId );
			if ( ! merged ) {

				throw new Error( 'PLOCBuilder: Missing worker merge result.' );

			}

			const parent = {
				id: nextId ++,
				faceLocal: - 1,
				minOriginal: merged.minOriginal,
				count: merged.count,
				bounds: Float64Array.from( merged.bounds ),
				left,
				right,
			};
			parentByEarly.set( early, parent );
			latePositions.add( late );
			pairRecords.push( { early, late } );

		}

		const nextClusters = [];
		for ( let i = 0; i < activeClusters; i ++ ) {

			if ( parentByEarly.has( i ) ) {

				nextClusters.push( parentByEarly.get( i ) );

			} else if ( ! latePositions.has( i ) ) {

				nextClusters.push( clusters[ i ] );

			}

		}

		rounds.push( {
			round: roundIndex ++,
			activeClusters,
			clusters: activeClusters,
			mutualPairs: pairRecords,
			mergedPairs: pairRecords.length,
			candidateEvaluations: candidateResult.candidateEvaluations,
			nextActiveClusters: nextClusters.length,
		} );
		clusters = nextClusters;

	}

	return { root: clusters[ 0 ] || null, rounds, order, stats };

}

function collectFaceLocals( root ) {

	const result = [];
	const stack = [ root ];
	while ( stack.length ) {

		const cluster = stack.pop();
		if ( cluster.faceLocal >= 0 ) {

			result.push( cluster.faceLocal );

		} else {

			// Push right first so left is popped first.
			stack.push( cluster.right, cluster.left );

		}

	}

	return result;

}

function longestAxis( bounds ) {

	const dx = bounds[ 3 ] - bounds[ 0 ];
	const dy = bounds[ 4 ] - bounds[ 1 ];
	const dz = bounds[ 5 ] - bounds[ 2 ];
	if ( dx >= dy && dx >= dz ) return 0;
	if ( dy >= dz ) return 1;
	return 2;

}

function writeEmptyRoot( bufferOffset, useSharedArrayBuffer ) {

	const BufferConstructor = useSharedArrayBuffer ? SharedArrayBuffer : ArrayBuffer;
	const buffer = new BufferConstructor( BYTES_PER_NODE );
	const float32Array = new Float32Array( buffer );
	const uint32Array = new Uint32Array( buffer );
	const uint16Array = new Uint16Array( buffer );
	float32Array[ 0 ] = Infinity;
	float32Array[ 1 ] = Infinity;
	float32Array[ 2 ] = Infinity;
	float32Array[ 3 ] = - Infinity;
	float32Array[ 4 ] = - Infinity;
	float32Array[ 5 ] = - Infinity;
	uint32Array[ 6 ] = bufferOffset;
	uint16Array[ 14 ] = 0;
	uint16Array[ 15 ] = IS_LEAFNODE_FLAG;
	return buffer;

}

/**
 * Packs a completed PLOC hierarchy into the library's native 32 byte nodes.
 * @param {object} params
 * @returns {{buffer:ArrayBuffer,permutation:number[],nodeCount:number}}
 */
export function packPLOCHierarchy( {
	root,
	faceCount,
	bufferOffset = 0,
	targetLeafSize = 10,
	maxDepth = 40,
	verbose = false,
	useSharedArrayBuffer = false,
} ) {

	if ( faceCount === 0 || ! root ) {

		return {
			buffer: writeEmptyRoot( bufferOffset, useSharedArrayBuffer ),
			permutation: [],
			nodeCount: 1,
		};

	}

	const BufferConstructor = useSharedArrayBuffer ? SharedArrayBuffer : ArrayBuffer;
	const buffer = new BufferConstructor( BYTES_PER_NODE * Math.max( 1, 2 * faceCount - 1 ) );
	const float32Array = new Float32Array( buffer );
	const uint32Array = new Uint32Array( buffer );
	const uint16Array = new Uint16Array( buffer );
	const permutation = [];
	let nodeCount = 0;
	let warnedMaxDepth = false;

	const stack = [ { type: 'enter', cluster: root, depth: 0 } ];
	while ( stack.length ) {

		const task = stack.pop();
		if ( task.type === 'afterLeft' ) {

			uint32Array[ task.node32 + 6 ] = nodeCount - task.nodeIndex;
			stack.push( { type: 'enter', cluster: task.right, depth: task.depth + 1 } );
			continue;

		}

		const { cluster, depth } = task;
		const nodeIndex = nodeCount ++;
		const node32 = nodeIndex * UINT32_PER_NODE;
		float32Array.set( cluster.bounds, node32 );

		if ( depth >= maxDepth && ! warnedMaxDepth ) {

			warnedMaxDepth = true;
			if ( verbose ) {

				console.warn( 'BVH: Max depth reached when generating PLOC BVH. Consider increasing maxDepth.' );

			}

		}

		const isLeafCluster = cluster.left === null;
		const truncate = isLeafCluster || cluster.count <= targetLeafSize || depth >= maxDepth;
		if ( truncate ) {

			const start = permutation.length;
			const locals = collectFaceLocals( cluster );
			for ( const local of locals ) permutation.push( local );
			uint32Array[ node32 + 6 ] = bufferOffset + start;
			uint16Array[ node32 * 2 + 14 ] = cluster.count;
			uint16Array[ node32 * 2 + 15 ] = IS_LEAFNODE_FLAG;
			continue;

		}

		uint32Array[ node32 + 7 ] = longestAxis( cluster.bounds );

		// Stack ordering: continuation is below left, and continuation pushes right.
		stack.push( {
			type: 'afterLeft',
			nodeIndex,
			node32,
			right: cluster.right,
			depth,
		} );
		stack.push( { type: 'enter', cluster: cluster.left, depth: depth + 1 } );

	}

	return {
		buffer: buffer.slice( 0, nodeCount * BYTES_PER_NODE ),
		permutation,
		nodeCount,
	};

}

function readRootFaces( bvh, range, bufferOffset ) {

	const count = range.count;
	const original = new Uint32Array( count );
	const bounds = new Float64Array( count * 6 );
	const indirectBuffer = bvh._indirectBuffer || null;

	for ( let local = 0; local < count; local ++ ) {

		const slot = bufferOffset + local;
		const faceIndex = indirectBuffer ? indirectBuffer[ slot ] : range.offset + local;
		original[ local ] = faceIndex;
		bvh.writePrimitiveBounds( slot, _tempBounds, 0 );
		bounds[ local * 6 + 0 ] = _tempBounds[ 0 ];
		bounds[ local * 6 + 1 ] = _tempBounds[ 1 ];
		bounds[ local * 6 + 2 ] = _tempBounds[ 2 ];
		bounds[ local * 6 + 3 ] = _tempBounds[ 3 ];
		bounds[ local * 6 + 4 ] = _tempBounds[ 4 ];
		bounds[ local * 6 + 5 ] = _tempBounds[ 5 ];

	}

	return { original, bounds, faceCount: count };

}

function getPLOCRootDescriptors( bvh, options ) {

	const ranges = getRootPrimitiveRanges( bvh.geometry, options.range, bvh.primitiveStride || 3 );
	const indirectBuffer = bvh._indirectBuffer || null;
	const descriptors = [];
	let indirectOffset = 0;
	for ( const range of ranges ) {

		descriptors.push( {
			range,
			bufferOffset: indirectBuffer ? indirectOffset : range.offset,
		} );
		indirectOffset += range.count;

	}

	return descriptors;

}

function applyPermutation( bvh, descriptor, permutation ) {

	const { range, bufferOffset } = descriptor;
	const count = range.count;
	const indirectBuffer = bvh._indirectBuffer;
	if ( indirectBuffer ) {

		const source = indirectBuffer.slice( bufferOffset, bufferOffset + count );
		for ( let target = 0; target < count; target ++ ) {

			indirectBuffer[ bufferOffset + target ] = source[ permutation[ target ] ];

		}

	} else {

		const index = bvh.geometry.index.array;
		const source = index.slice( range.offset * 3, ( range.offset + count ) * 3 );
		for ( let target = 0; target < count; target ++ ) {

			const sourceBase = permutation[ target ] * 3;
			const targetBase = ( range.offset + target ) * 3;
			index[ targetBase + 0 ] = source[ sourceBase + 0 ];
			index[ targetBase + 1 ] = source[ sourceBase + 1 ];
			index[ targetBase + 2 ] = source[ sourceBase + 2 ];

		}

	}

}

/**
 * Synchronous MeshBVH build entry point used by `strategy: PLOC`.
 * @param {object} bvh
 * @param {object} options
 * @returns {Array<ArrayBuffer>}
 */
export function buildPLOCPackedTree( bvh, options ) {

	if ( options.strategy !== PLOC ) {

		throw new Error( 'PLOCBuilder: buildPLOCPackedTree() requires strategy PLOC.' );

	}

	const descriptors = getPLOCRootDescriptors( bvh, options );
	const roots = [];
	for ( const descriptor of descriptors ) {

		const rootFaces = readRootFaces( bvh, descriptor.range, descriptor.bufferOffset );
		const { root } = clusterPLOCSerial( rootFaces );
		const packed = packPLOCHierarchy( {
			root,
			faceCount: descriptor.range.count,
			bufferOffset: descriptor.bufferOffset,
			targetLeafSize: options.targetLeafSize,
			maxDepth: options.maxDepth,
			verbose: options.verbose,
			useSharedArrayBuffer: options.useSharedArrayBuffer,
		} );
		applyPermutation( bvh, descriptor, packed.permutation );
		roots.push( packed.buffer );

	}

	bvh._roots = roots;
	return roots;

}

/**
 * Parallel MeshBVH build used by NodePLOCWorker. Candidate and merge batches are
 * executed through the supplied worker executor.
 * @param {object} bvh
 * @param {object} options
 * @param {(name:string,payload:object)=>Promise<object>} executor
 * @returns {Promise<{roots:Array<ArrayBuffer>,stats:object}>}
 */
export async function buildPLOCPackedTreeParallel( bvh, options, executor ) {

	const descriptors = getPLOCRootDescriptors( bvh, options );
	const roots = [];
	const stats = {
		rounds: [],
		roots: [],
		candidateEvaluations: 0,
		mergedPairs: 0,
	};

	for ( let rootIndex = 0; rootIndex < descriptors.length; rootIndex ++ ) {

		const descriptor = descriptors[ rootIndex ];
		const rootFaces = readRootFaces( bvh, descriptor.range, descriptor.bufferOffset );
		const result = await clusterPLOCParallel( rootFaces, executor );
		const packed = packPLOCHierarchy( {
			root: result.root,
			faceCount: descriptor.range.count,
			bufferOffset: descriptor.bufferOffset,
			targetLeafSize: options.targetLeafSize,
			maxDepth: options.maxDepth,
			verbose: options.verbose,
			useSharedArrayBuffer: options.useSharedArrayBuffer,
		} );
		applyPermutation( bvh, descriptor, packed.permutation );
		roots.push( packed.buffer );

		stats.candidateEvaluations += result.stats.candidateEvaluations;
		stats.mergedPairs += result.stats.mergedPairs;
		stats.roots.push( {
			rootIndex,
			faceCount: descriptor.range.count,
			rounds: result.rounds,
			activeClustersPerRound: result.rounds.map( round => round.activeClusters ),
		} );
		for ( const round of result.rounds ) {

			stats.rounds.push( { ...round, rootIndex } );

		}

	}

	stats.activeClustersPerRound = stats.rounds.map( round => round.activeClusters );
	bvh._roots = roots;
	return { roots, stats };

}

/**
 * Convenience namespace export for code that prefers a named builder object.
 */
export const PLOCBuilder = {
	PLOC,
	expandMorton10,
	morton3D10,
	quantizeCentroid10,
	surfaceArea6,
	unionBounds6,
	unionSurfaceArea6,
	evaluateCandidateAt,
	evaluateCandidateBatch,
	mergeClusterPair,
	mergeClusterPairBatch,
	clusterPLOCSerial,
	clusterPLOCParallel,
	packPLOCHierarchy,
	buildPLOCPackedTree,
	buildPLOCPackedTreeParallel,
};

export default PLOCBuilder;
