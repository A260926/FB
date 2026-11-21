import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { execFileSync } from 'node:child_process';
import {
	BufferGeometry,
	Float32BufferAttribute,
	Mesh,
	MeshBasicMaterial,
	Raycaster,
	Ray,
	Vector3,
	Box3,
	DoubleSide,
	FrontSide,
} from 'three';
import { MeshBVH } from '../src/core/MeshBVH.js';
import { PLOC, SKIP_GENERATION } from '../src/core/Constants.js';
import { generateIndirectBuffer } from '../src/core/GeometryBVH.js';
import { getRootPrimitiveRanges } from '../src/core/build/geometryUtils.js';
import { acceleratedRaycast } from '../src/utils/ExtensionUtilities.js';
import {
	PLOC_NEIGHBOR_WINDOW,
	preparePLOCState,
	runPLOCClusteringSync,
	layoutPLOCTree,
	createPLOCStats,
} from '../src/core/build/PLOCBuilder.js';
import { NodePLOCWorker } from '../src/workers/NodePLOCWorker.js';
import {
	createPreviewGeometry,
	renderPreview,
	encodePPM,
} from '../example/ploc.mjs';

const REPO_ROOT = path.resolve( path.dirname( new URL( import.meta.url ).pathname ), '..' );

/* -------------------------------------------------------------------------- */
/* small utilities                                                            */
/* -------------------------------------------------------------------------- */

function mulberry32( seed ) {

	let a = seed >>> 0;
	return function () {

		a |= 0;
		a = ( a + 0x6d2b79f5 ) | 0;
		let t = Math.imul( a ^ ( a >>> 15 ), 1 | a );
		t = ( t + Math.imul( t ^ ( t >>> 7 ), 61 | t ) ) ^ t;
		return ( ( t ^ ( t >>> 14 ) ) >>> 0 ) / 4294967296;

	};

}

function bufferBytes( buffer ) {

	return Buffer.from( buffer );

}

function typedBytes( array ) {

	return Buffer.from( array.buffer, array.byteOffset, array.byteLength );

}

// decodes a packed node buffer into a pre-order node list
function decodePacked( buffer ) {

	const uint32Array = new Uint32Array( buffer );
	const uint16Array = new Uint16Array( buffer );
	const float32Array = new Float32Array( buffer );
	const nodes = [];
	visit( 0, 0 );
	return nodes;

	function visit( index, depth ) {

		const n32 = index * 8;
		const n16 = n32 * 2;
		const bounds = Array.from( float32Array.subarray( n32, n32 + 6 ) );
		if ( uint16Array[ n16 + 15 ] === 0xffff ) {

			nodes.push( {
				depth,
				bounds,
				leaf: true,
				offset: uint32Array[ n32 + 6 ],
				count: uint16Array[ n16 + 14 ],
			} );

		} else {

			const entry = {
				depth,
				bounds,
				leaf: false,
				right: uint32Array[ n32 + 6 ],
				splitAxis: uint32Array[ n32 + 7 ],
			};
			nodes.push( entry );
			visit( index + 1, depth + 1 );
			visit( index + entry.right, depth + 1 );

		}

	}

}

/* -------------------------------------------------------------------------- */
/* independent serial PLOC reference, implemented directly from the spec:      */
/* morton ordered leaves, then rounds of mutually-nearest merging with a       */
/* +/-16 neighbor window, union surface area cost and min-face tie breaking    */
/* -------------------------------------------------------------------------- */

function refExpandBits( v ) {

	v = ( v | ( v << 16 ) ) & 0x030000ff;
	v = ( v | ( v << 8 ) ) & 0x0300f00f;
	v = ( v | ( v << 4 ) ) & 0x030c30c3;
	v = ( v | ( v << 2 ) ) & 0x09249249;
	return v;

}

function refQuantize( c, min, span ) {

	if ( span === 0 ) {

		return 0;

	}

	return Math.min( 1023, Math.floor( 1024 * ( ( c - min ) / span ) ) );

}

function refUnionBounds( a, b ) {

	const out = new Array( 6 );
	for ( let j = 0; j < 6; j ++ ) {

		out[ j ] = j < 3 ? ( a[ j ] < b[ j ] ? a[ j ] : b[ j ] ) : ( a[ j ] > b[ j ] ? a[ j ] : b[ j ] );

	}

	return out;

}

function refUnionArea( a, b ) {

	const minX = a[ 0 ] < b[ 0 ] ? a[ 0 ] : b[ 0 ];
	const minY = a[ 1 ] < b[ 1 ] ? a[ 1 ] : b[ 1 ];
	const minZ = a[ 2 ] < b[ 2 ] ? a[ 2 ] : b[ 2 ];
	const maxX = a[ 3 ] > b[ 3 ] ? a[ 3 ] : b[ 3 ];
	const maxY = a[ 4 ] > b[ 4 ] ? a[ 4 ] : b[ 4 ];
	const maxZ = a[ 5 ] > b[ 5 ] ? a[ 5 ] : b[ 5 ];
	const d0 = maxX - minX;
	const d1 = maxY - minY;
	const d2 = maxZ - minZ;
	return 2 * ( d0 * d1 + d1 * d2 + d2 * d0 );

}

// primitiveBounds: Float32Array of [ center, half ] x 3 per primitive
function referenceCluster( primitiveBounds, count ) {

	// centroid range
	let cminx = Infinity, cminy = Infinity, cminz = Infinity;
	let cmaxx = - Infinity, cmaxy = - Infinity, cmaxz = - Infinity;
	for ( let i = 0; i < count; i ++ ) {

		const cx = primitiveBounds[ i * 6 + 0 ];
		const cy = primitiveBounds[ i * 6 + 2 ];
		const cz = primitiveBounds[ i * 6 + 4 ];
		if ( cx < cminx ) cminx = cx;
		if ( cx > cmaxx ) cmaxx = cx;
		if ( cy < cminy ) cminy = cy;
		if ( cy > cmaxy ) cmaxy = cy;
		if ( cz < cminz ) cminz = cz;
		if ( cz > cmaxz ) cmaxz = cz;

	}

	// morton code per primitive from the normalized center
	const codes = new Array( count );
	for ( let i = 0; i < count; i ++ ) {

		const qx = refQuantize( primitiveBounds[ i * 6 + 0 ], cminx, cmaxx - cminx );
		const qy = refQuantize( primitiveBounds[ i * 6 + 2 ], cminy, cmaxy - cminy );
		const qz = refQuantize( primitiveBounds[ i * 6 + 4 ], cminz, cmaxz - cminz );
		codes[ i ] = refExpandBits( qx ) | ( refExpandBits( qy ) << 1 ) | ( refExpandBits( qz ) << 2 );

	}

	// stable order by morton code, ties broken by original face order
	const order = [ ...Array( count ).keys() ].sort( ( a, b ) => ( codes[ a ] - codes[ b ] ) || ( a - b ) );

	// leaf clusters in morton order
	let clusters = order.map( index => ( {
		bounds: [
			primitiveBounds[ index * 6 + 0 ] - primitiveBounds[ index * 6 + 1 ],
			primitiveBounds[ index * 6 + 2 ] - primitiveBounds[ index * 6 + 3 ],
			primitiveBounds[ index * 6 + 4 ] - primitiveBounds[ index * 6 + 5 ],
			primitiveBounds[ index * 6 + 0 ] + primitiveBounds[ index * 6 + 1 ],
			primitiveBounds[ index * 6 + 2 ] + primitiveBounds[ index * 6 + 3 ],
			primitiveBounds[ index * 6 + 4 ] + primitiveBounds[ index * 6 + 5 ],
		],
		minFace: index,
		faces: [ index ],
		children: null,
	} ) );

	const rounds = [];
	const pairsPerRound = [];
	while ( clusters.length > 1 ) {

		const m = clusters.length;
		rounds.push( m );

		// nearest neighbor proposals over the frozen order
		const best = new Array( m );
		for ( let p = 0; p < m; p ++ ) {

			let bestQ = - 1;
			let bestArea = Infinity;
			let bestFace = Infinity;
			const lo = Math.max( 0, p - PLOC_NEIGHBOR_WINDOW );
			const hi = Math.min( m - 1, p + PLOC_NEIGHBOR_WINDOW );
			for ( let q = lo; q <= hi; q ++ ) {

				if ( q === p ) {

					continue;

				}

				const area = refUnionArea( clusters[ p ].bounds, clusters[ q ].bounds );
				const face = clusters[ q ].minFace;
				if ( area < bestArea || ( area === bestArea && face < bestFace ) ) {

					bestArea = area;
					bestFace = face;
					bestQ = q;

				}

			}

			best[ p ] = bestQ;

		}

		// mutually nearest pairs merge simultaneously
		const pairs = [];
		for ( let p = 0; p < m; p ++ ) {

			const q = best[ p ];
			if ( q > p && best[ q ] === p ) {

				pairs.push( [ p, q ] );

			}

		}

		if ( pairs.length === 0 ) {

			for ( let p = 0; p + 1 < m; p += 2 ) {

				pairs.push( [ p, p + 1 ] );

			}

		}

		pairsPerRound.push( pairs );

		// parents take the earlier slot, children stay in pre-merge order
		const parentAt = new Array( m ).fill( null );
		const removed = new Set();
		for ( const [ p, q ] of pairs ) {

			const A = clusters[ p ];
			const B = clusters[ q ];
			parentAt[ p ] = {
				bounds: refUnionBounds( A.bounds, B.bounds ),
				minFace: Math.min( A.minFace, B.minFace ),
				faces: A.faces.concat( B.faces ),
				children: [ A, B ],
			};
			removed.add( q );

		}

		const next = [];
		for ( let p = 0; p < m; p ++ ) {

			if ( removed.has( p ) ) {

				continue;

			}

			next.push( parentAt[ p ] === null ? clusters[ p ] : parentAt[ p ] );

		}

		clusters = next;

	}

	rounds.push( clusters.length );
	return { rounds, pairsPerRound, tree: clusters[ 0 ], initialOrder: order };

}

// depth first layout of the reference tree with targetLeafSize / maxDepth folding
function referenceLayout( tree, targetLeafSize, maxDepth ) {

	const nodes = [];
	const faces = [];
	write( tree, 0 );
	return { nodes, faces };

	function emit( node ) {

		if ( node.children === null ) {

			faces.push( node.faces[ 0 ] );
			return;

		}

		emit( node.children[ 0 ] );
		emit( node.children[ 1 ] );

	}

	function write( node, depth ) {

		const index = nodes.length;
		const entry = {
			bounds: node.bounds.map( v => Math.fround( v ) ),
			leaf: false,
			offset: 0,
			count: 0,
			right: 0,
			splitAxis: 0,
			depth,
		};
		nodes.push( entry );

		const count = node.faces.length;
		if ( count <= targetLeafSize || depth >= maxDepth || node.children === null ) {

			entry.leaf = true;
			entry.offset = faces.length;
			entry.count = count;
			emit( node );
			return;

		}

		write( node.children[ 0 ], depth + 1 );
		entry.right = nodes.length - index;
		write( node.children[ 1 ], depth + 1 );

	}

}

/* -------------------------------------------------------------------------- */
/* shared comparison helpers                                                  */
/* -------------------------------------------------------------------------- */

function compareClusterTrees( check, state, id, refNode, path ) {

	let boundsEqual = true;
	for ( let j = 0; j < 6; j ++ ) {

		if ( state.bounds[ id * 6 + j ] !== refNode.bounds[ j ] ) {

			boundsEqual = false;

		}

	}

	check( boundsEqual, `${ path }: cluster bounds match` );
	check( state.minFace[ id ] === refNode.minFace, `${ path }: min face matches` );
	check( state.primCount[ id ] === refNode.faces.length, `${ path }: primitive count matches` );
	if ( refNode.children === null ) {

		check( state.left[ id ] === - 1, `${ path }: leaf cluster` );
		check( state.leafIndex[ id ] === refNode.faces[ 0 ], `${ path }: leaf face matches` );

	} else {

		check( state.left[ id ] !== - 1, `${ path }: internal cluster` );
		compareClusterTrees( check, state, state.left[ id ], refNode.children[ 0 ], `${ path }.l` );
		compareClusterTrees( check, state, state.right[ id ], refNode.children[ 1 ], `${ path }.r` );

	}

}

function comparePackedToReference( check, buffer, ref, label ) {

	const nodes = decodePacked( buffer );
	check( nodes.length === ref.nodes.length, `${ label }: node count ${ nodes.length } == ${ ref.nodes.length }` );
	const total = Math.min( nodes.length, ref.nodes.length );
	for ( let i = 0; i < total; i ++ ) {

		const a = nodes[ i ];
		const b = ref.nodes[ i ];
		check( a.leaf === b.leaf, `${ label }: node ${ i } leaf flag` );
		check( a.bounds.every( ( v, j ) => v === b.bounds[ j ] ), `${ label }: node ${ i } bounds` );
		if ( a.leaf && b.leaf ) {

			check( a.offset === b.offset && a.count === b.count, `${ label }: node ${ i } leaf range` );

		} else if ( ! a.leaf && ! b.leaf ) {

			check( a.right === b.right && a.splitAxis === b.splitAxis, `${ label }: node ${ i } interior` );

		}

	}

}

// runs production clustering + layout for the given primitive bounds and compares
// every round and the final packed tree against the independent reference
function runClusteringComparison( check, label, primitiveBounds, count, layouts ) {

	const reference = referenceCluster( primitiveBounds, count );
	const state = preparePLOCState( primitiveBounds, 0, count );
	const stats = runPLOCClusteringSync( state, createPLOCStats() );

	// initial morton + face order
	check(
		Array.from( state.leafIndex ).join( ',' ) === reference.initialOrder.join( ',' ),
		`${ label }: initial morton order matches`,
	);

	// rounds and pairings
	check( stats.rounds.join( ',' ) === reference.rounds.join( ',' ), `${ label }: round counts match` );
	check( stats.roundPairs.length === reference.pairsPerRound.length, `${ label }: round pair list length` );
	for ( let r = 0; r < reference.pairsPerRound.length; r ++ ) {

		const expected = reference.pairsPerRound[ r ].map( p => p.join( ':' ) ).join( ',' );
		const actual = ( stats.roundPairs[ r ] || [] ).map( p => p.join( ':' ) ).join( ',' );
		check( actual === expected, `${ label }: pairs of round ${ r } match` );

	}

	// final cluster hierarchy
	compareClusterTrees( check, state, state.rootId, reference.tree, label );

	// packed layouts under different truncation settings
	for ( const { targetLeafSize, maxDepth } of layouts ) {

		const fakeIndex = new Uint32Array( count * 3 );
		for ( let t = 0; t < count; t ++ ) {

			fakeIndex[ t * 3 + 0 ] = t * 3 + 0;
			fakeIndex[ t * 3 + 1 ] = t * 3 + 1;
			fakeIndex[ t * 3 + 2 ] = t * 3 + 2;

		}

		const fakeBvh = { primitiveBuffer: fakeIndex, primitiveBufferStride: 3 };
		const buffer = layoutPLOCTree(
			fakeBvh,
			state,
			0,
			count,
			{ targetLeafSize, maxDepth, verbose: false },
			{ offset: 0, count },
		);

		const refLayout = referenceLayout( reference.tree, targetLeafSize, maxDepth );
		comparePackedToReference( check, buffer, refLayout, `${ label } (leaf ${ targetLeafSize }, depth ${ maxDepth })` );

		// face slot permutation matches the reference depth first order
		const expectedFaces = [];
		for ( const face of refLayout.faces ) {

			expectedFaces.push( face * 3, face * 3 + 1, face * 3 + 2 );

		}

		check(
			Array.from( fakeIndex ).join( ',' ) === expectedFaces.join( ',' ),
			`${ label } (leaf ${ targetLeafSize }, depth ${ maxDepth }): face slots`,
		);

	}

}

/* -------------------------------------------------------------------------- */
/* geometry and ray helpers                                                   */
/* -------------------------------------------------------------------------- */

function makeGridGeometry( nx, ny, options = {} ) {

	const { wave = true, shuffleSeed = null, indexed = true } = options;
	const vertices = [];
	const uvs = [];
	for ( let y = 0; y <= ny; y ++ ) {

		for ( let x = 0; x <= nx; x ++ ) {

			const px = ( x / nx - 0.5 ) * 2;
			const py = ( y / ny - 0.5 ) * 2;
			const pz = wave ? 0.3 * Math.sin( px * 2.4 ) * Math.cos( py * 2.1 ) + 0.1 * Math.sin( ( px + py ) * 3.7 ) : 0;
			vertices.push( px, py, pz );
			uvs.push( x / nx, y / ny );

		}

	}

	const indices = [];
	for ( let y = 0; y < ny; y ++ ) {

		for ( let x = 0; x < nx; x ++ ) {

			const a = y * ( nx + 1 ) + x;
			const b = a + 1;
			const c = a + nx + 1;
			const d = c + 1;
			indices.push( a, b, d, a, d, c );

		}

	}

	const geometry = new BufferGeometry();
	if ( indexed ) {

		geometry.setAttribute( 'position', new Float32BufferAttribute( vertices, 3 ) );
		geometry.setAttribute( 'uv', new Float32BufferAttribute( uvs, 2 ) );
		geometry.setIndex( indices );

	} else {

		const expandedVertices = [];
		const expandedUvs = [];
		for ( const i of indices ) {

			expandedVertices.push( vertices[ i * 3 ], vertices[ i * 3 + 1 ], vertices[ i * 3 + 2 ] );
			expandedUvs.push( uvs[ i * 2 ], uvs[ i * 2 + 1 ] );

		}

		geometry.setAttribute( 'position', new Float32BufferAttribute( expandedVertices, 3 ) );
		geometry.setAttribute( 'uv', new Float32BufferAttribute( expandedUvs, 2 ) );

	}

	if ( shuffleSeed !== null ) {

		shuffleTriangles( geometry, shuffleSeed );

	}

	return geometry;

}

function shuffleTriangles( geometry, seed ) {

	const rand = mulberry32( seed );
	const index = geometry.index.array;
	const triCount = index.length / 3;
	const order = [ ...Array( triCount ).keys() ];
	for ( let i = order.length - 1; i > 0; i -- ) {

		const j = Math.floor( rand() * ( i + 1 ) );
		const t = order[ i ];
		order[ i ] = order[ j ];
		order[ j ] = t;

	}

	const copy = index.slice();
	for ( let t = 0; t < triCount; t ++ ) {

		for ( let k = 0; k < 3; k ++ ) {

			index[ t * 3 + k ] = copy[ order[ t ] * 3 + k ];

		}

	}

}

// deterministic set of rays: jittered downward rays, slanted rays, and misses
function makeRays( seed, count = 24 ) {

	const rand = mulberry32( seed );
	const rays = [];
	for ( let i = 0; i < count; i ++ ) {

		if ( i % 4 === 3 ) {

			const origin = new Vector3( rand() * 4 - 2, rand() * 4 - 2, 2 + rand() );
			const direction = new Vector3( rand() - 0.5, rand() - 0.5, - 1 - rand() ).normalize();
			rays.push( new Ray( origin, direction ) );

		} else {

			const x = - 1.2 + 2.4 * ( ( i * 0.6180339887498949 ) % 1 ) + 0.00123;
			const y = - 1.2 + 2.4 * ( ( i * 0.41421356237309503 ) % 1 ) + 0.00456;
			rays.push( new Ray( new Vector3( x, y, 3 ), new Vector3( 0, 0, - 1 ) ) );

		}

	}

	rays.push( new Ray( new Vector3( 50, 50, 3 ), new Vector3( 0, 0, - 1 ) ) );
	rays.push( new Ray( new Vector3( 0, 0, 3 ), new Vector3( 0, 0, 1 ) ) );
	return rays;

}

// original three.js per-triangle raycast, no BVH involved
function bruteForceHits( geometry, material, ray ) {

	const mesh = new Mesh( geometry, material );
	const raycaster = new Raycaster( ray.origin, ray.direction );
	const hits = [];
	Mesh.prototype.raycast.call( mesh, raycaster, hits );
	hits.sort( ( a, b ) => a.distance - b.distance );
	return hits;

}

function bvhFirstHit( geometry, material, ray ) {

	const mesh = new Mesh( geometry, material );
	const raycaster = new Raycaster( ray.origin, ray.direction );
	raycaster.firstHitOnly = true;
	const hits = [];
	acceleratedRaycast.call( mesh, raycaster, hits );
	return hits.length ? hits[ 0 ] : null;

}

function compareRay( check, label, geometry, material, ray ) {

	const refHits = bruteForceHits( geometry, material, ray );
	const hit = bvhFirstHit( geometry, material, ray );
	const ref = refHits.length ? refHits[ 0 ] : null;
	if ( ref === null ) {

		check( hit === null, `${ label }: no hit expected` );
		return;

	}

	check( hit !== null, `${ label }: hit expected` );
	if ( hit === null ) {

		return;

	}

	const tolerance = 1e-6 * Math.max( 1, Math.abs( ref.distance ) );
	check( Math.abs( hit.distance - ref.distance ) <= tolerance, `${ label }: distance ${ hit.distance } ~= ${ ref.distance }` );

	// exact distance ties are ambiguous for any traversal order, so only compare the
	// identity of the hit when the reference has a single closest triangle
	const tied = refHits.filter( h => Math.abs( h.distance - ref.distance ) <= 1e-9 );
	if ( tied.length === 1 ) {

		check( hit.faceIndex === ref.faceIndex, `${ label }: faceIndex ${ hit.faceIndex } == ${ ref.faceIndex }` );
		if ( ref.uv ) {

			check(
				hit.uv && Math.abs( hit.uv.x - ref.uv.x ) < 1e-6 && Math.abs( hit.uv.y - ref.uv.y ) < 1e-6,
				`${ label }: uv matches`,
			);

		}

		const refMaterial = ref.face ? ref.face.materialIndex : 0;
		const gotMaterial = hit.face ? hit.face.materialIndex : 0;
		check( gotMaterial === refMaterial, `${ label }: materialIndex ${ gotMaterial } == ${ refMaterial }` );

	} else {

		check( tied.some( h => h.faceIndex === hit.faceIndex ), `${ label }: tied faceIndex is one of the closest` );

	}

}

function compareRaySet( check, label, geometry, material, rays ) {

	for ( let i = 0; i < rays.length; i ++ ) {

		compareRay( check, `${ label } ray ${ i }`, geometry, material, rays[ i ] );

	}

}

function sortedTriangleIds( geometry, startTri, triCount ) {

	const index = geometry.index.array;
	const ids = [];
	for ( let t = startTri; t < startTri + triCount; t ++ ) {

		ids.push( [ index[ t * 3 ], index[ t * 3 + 1 ], index[ t * 3 + 2 ] ].join( ',' ) );

	}

	return ids.sort();

}

/* -------------------------------------------------------------------------- */
/* clustering cases                                                           */
/* -------------------------------------------------------------------------- */

function makePrimitiveBounds( list ) {

	const bounds = new Float32Array( list.length * 6 );
	for ( let i = 0; i < list.length; i ++ ) {

		bounds.set( list[ i ], i * 6 );

	}

	return bounds;

}

function randomPrimitiveBounds( seed, count, halfMax = 0.3 ) {

	const rand = mulberry32( seed );
	const list = [];
	for ( let i = 0; i < count; i ++ ) {

		list.push( [
			rand() * 4 - 2, rand() * halfMax + 0.001,
			rand() * 4 - 2, rand() * halfMax + 0.001,
			rand() * 4 - 2, rand() * halfMax + 0.001,
		] );

	}

	return makePrimitiveBounds( list );

}

const LAYOUTS = [
	{ targetLeafSize: 10, maxDepth: 40 },
	{ targetLeafSize: 3, maxDepth: 40 },
	{ targetLeafSize: 2, maxDepth: 4 },
];

const clusteringCases = [
	{
		group: 'clustering',
		name: 'random triangles match the serial reference in every round',
		run( check ) {

			const bounds = randomPrimitiveBounds( 11, 48 );
			runClusteringComparison( check, 'random', bounds, 48, LAYOUTS );

		},
	},
	{
		group: 'clustering',
		name: 'identical morton codes fall back to original face order',
		run( check ) {

			const list = [];
			// two groups of triangles sharing the exact same center (and morton code)
			for ( let i = 0; i < 6; i ++ ) {

				list.push( [ 0.5, 0.05 + i * 0.01, 0.5, 0.04, 0.5, 0.03 ] );

			}

			for ( let i = 0; i < 4; i ++ ) {

				list.push( [ - 1, 0.02, 0, 0.02, 2, 0.02 ] );

			}

			// exact duplicates, including duplicate bounds
			list.push( [ 0.25, 0.1, - 0.5, 0.1, 1, 0.1 ] );
			list.push( [ 0.25, 0.1, - 0.5, 0.1, 1, 0.1 ] );
			const rand = mulberry32( 23 );
			for ( let i = 0; i < 9; i ++ ) {

				list.push( [
					rand() * 4 - 2, 0.05,
					rand() * 4 - 2, 0.05,
					rand() * 4 - 2, 0.05,
				] );

			}

			const bounds = makePrimitiveBounds( list );
			runClusteringComparison( check, 'same-code', bounds, list.length, LAYOUTS );

			// duplicates keep their relative face order in the initial sequence
			const state = preparePLOCState( bounds, 0, list.length );
			const first = Array.from( state.leafIndex ).indexOf( 10 );
			const second = Array.from( state.leafIndex ).indexOf( 11 );
			check( first < second, 'same-code: duplicate faces stay in original order' );

		},
	},
	{
		group: 'clustering',
		name: 'planar geometry with a zero-span axis',
		run( check ) {

			const rand = mulberry32( 5 );
			const list = [];
			for ( let i = 0; i < 26; i ++ ) {

				list.push( [
					rand() * 4 - 2, rand() * 0.2 + 0.001,
					rand() * 4 - 2, rand() * 0.2 + 0.001,
					1.25, 0, // zero z span
				] );

			}

			const bounds = makePrimitiveBounds( list );
			runClusteringComparison( check, 'planar', bounds, list.length, LAYOUTS );

		},
	},
	{
		group: 'clustering',
		name: 'asymmetric clusters of mixed sizes',
		run( check ) {

			const list = [ [ 0, 10, 0, 10, 0, 10 ] ];
			const rand = mulberry32( 17 );
			for ( let i = 0; i < 20; i ++ ) {

				list.push( [
					100 + rand() * 4, 0.01,
					100 + rand() * 4, 0.01,
					100 + rand() * 4, 0.01,
				] );

			}

			for ( let i = 0; i < 3; i ++ ) {

				list.push( [ - 50 - i, 0.5, - 50, 0.5, - 50, 0.5 ] );

			}

			const bounds = makePrimitiveBounds( list );
			runClusteringComparison( check, 'asymmetric', bounds, list.length, LAYOUTS );

			// the root bounds equal the union of all primitive bounds
			const state = preparePLOCState( bounds, 0, list.length );
			runPLOCClusteringSync( state );
			let ok = true;
			for ( let axis = 0; axis < 3; axis ++ ) {

				let min = Infinity, max = - Infinity;
				for ( let i = 0; i < list.length; i ++ ) {

					const c = bounds[ i * 6 + axis * 2 ];
					const h = bounds[ i * 6 + axis * 2 + 1 ];
					min = Math.min( min, c - h );
					max = Math.max( max, c + h );

				}

				if ( state.bounds[ state.rootId * 6 + axis ] !== min || state.bounds[ state.rootId * 6 + 3 + axis ] !== max ) {

					ok = false;

				}

			}

			check( ok, 'asymmetric: root bounds are the union of all primitives' );

		},
	},
	{
		group: 'clustering',
		name: 'single triangle produces a single leaf node',
		run( check ) {

			const bounds = makePrimitiveBounds( [ [ 0.5, 0.25, - 1, 0.5, 2, 0.125 ] ] );
			const state = preparePLOCState( bounds, 0, 1 );
			const stats = runPLOCClusteringSync( state, createPLOCStats() );
			check( stats.rounds.join( ',' ) === '1', 'single: one round entry' );
			check( stats.mergedPairs === 0, 'single: no merges' );
			check( stats.candidateEvaluations === 0, 'single: no candidates' );

			const fakeIndex = new Uint32Array( 3 );
			fakeIndex.set( [ 0, 1, 2 ] );
			const buffer = layoutPLOCTree(
				{ primitiveBuffer: fakeIndex, primitiveBufferStride: 3 },
				state,
				0,
				1,
				{ targetLeafSize: 10, maxDepth: 40, verbose: false },
				{ offset: 0, count: 1 },
			);
			const nodes = decodePacked( buffer );
			check( nodes.length === 1, 'single: one packed node' );
			check( nodes[ 0 ].leaf && nodes[ 0 ].count === 1 && nodes[ 0 ].offset === 0, 'single: leaf node' );
			check(
				nodes[ 0 ].bounds[ 0 ] === Math.fround( 0.5 - 0.25 ) && nodes[ 0 ].bounds[ 3 ] === Math.fround( 0.5 + 0.25 ),
				'single: leaf bounds',
			);

			// end to end through MeshBVH
			const geometry = new BufferGeometry();
			geometry.setAttribute( 'position', new Float32BufferAttribute( [ 0, 0, 0, 1, 0, 0, 0, 1, 0 ], 3 ) );
			const bvh = new MeshBVH( geometry, { strategy: PLOC } );
			const hit = bvh.raycastFirst( new Ray( new Vector3( 0.1, 0.1, 2 ), new Vector3( 0, 0, - 1 ) ), DoubleSide );
			check( hit !== null && hit.distance === 2 && hit.faceIndex === 0, 'single: raycastFirst hits' );

		},
	},
	{
		group: 'clustering',
		name: 'empty geometry produces a queryable empty tree',
		run( check ) {

			const geometry = new BufferGeometry();
			geometry.setAttribute( 'position', new Float32BufferAttribute( [], 3 ) );
			const bvh = new MeshBVH( geometry, { strategy: PLOC } );
			check( bvh._roots.length === 1, 'empty: one root' );

			const ray = new Ray( new Vector3( 0, 0, 2 ), new Vector3( 0, 0, - 1 ) );
			check( bvh.raycastFirst( ray, DoubleSide ) === null, 'empty: raycastFirst is null' );
			check( bvh.raycast( ray, DoubleSide ).length === 0, 'empty: raycast finds nothing' );

			let nodeVisits = 0;
			bvh.traverse( () => nodeVisits ++ );
			check( nodeVisits === 1, 'empty: single node' );

			check( bvh.getBoundingBox( new Box3() ).isEmpty(), 'empty: bounding box is empty' );
			const castResult = bvh.shapecast( {
				intersectsBounds: () => true,
				intersectsTriangle: () => true,
			} );
			check( castResult === false, 'empty: shapecast finds nothing' );

		},
	},
	{
		group: 'clustering',
		name: 'stride-1 partition buffers are permuted like the reference',
		run( check ) {

			const bounds = randomPrimitiveBounds( 31, 37 );
			const reference = referenceCluster( bounds, 37 );
			const state = preparePLOCState( bounds, 0, 37 );
			runPLOCClusteringSync( state );

			const indirectLike = new Uint32Array( 37 );
			for ( let i = 0; i < 37; i ++ ) indirectLike[ i ] = i;
			layoutPLOCTree(
				{ primitiveBuffer: indirectLike, primitiveBufferStride: 1 },
				state,
				0,
				37,
				{ targetLeafSize: 4, maxDepth: 40, verbose: false },
				{ offset: 0, count: 37 },
			);

			const refLayout = referenceLayout( reference.tree, 4, 40 );
			check(
				Array.from( indirectLike ).join( ',' ) === refLayout.faces.join( ',' ),
				'stride-1: indirect slots follow the reference face order',
			);

		},
	},
];

/* -------------------------------------------------------------------------- */
/* parallel cases                                                             */
/* -------------------------------------------------------------------------- */

// computes the same primitive bounds the builds operate on, for serial stats
function computeBuildPrimitiveBounds( geometry, indirect ) {

	const proxy = new MeshBVH( geometry, { [ SKIP_GENERATION ]: true, indirect } );
	if ( indirect ) {

		proxy._indirectBuffer = generateIndirectBuffer( getRootPrimitiveRanges( geometry, null, 3 ), false );

	}

	const rootRanges = proxy.getRootRanges( null );
	const first = rootRanges[ 0 ];
	const last = rootRanges[ rootRanges.length - 1 ];
	const fullRange = { offset: first.offset, count: last.offset + last.count - first.offset };
	const primitiveBounds = new Float32Array( 6 * fullRange.count );
	primitiveBounds.offset = fullRange.offset;
	proxy.computePrimitiveBounds( fullRange.offset, fullRange.count, primitiveBounds );
	return { primitiveBounds, rootRanges };

}

function serialStatsFor( geometry, indirect ) {

	const { primitiveBounds, rootRanges } = computeBuildPrimitiveBounds( geometry, indirect );
	const stats = createPLOCStats();
	for ( const range of rootRanges ) {

		if ( range.count < 1 ) {

			continue;

		}

		runPLOCClusteringSync( preparePLOCState( primitiveBounds, range.offset, range.count ), stats );

	}

	return stats;

}

const parallelCases = [
	{
		group: 'parallel',
		name: '8192-face build is byte identical across 1, 2, and 4 workers and the sync build',
		async run( check ) {

			const base = createPreviewGeometry( 64 );
			check( base.index.count / 3 === 8192, 'sample has 8192 faces' );

			for ( const indirect of [ false, true ] ) {

				const label = `indirect=${ indirect }`;
				const syncGeometry = base.clone();
				const syncBvh = new MeshBVH( syncGeometry, { strategy: PLOC, indirect, targetLeafSize: 6 } );
				const serialStats = serialStatsFor( base.clone(), indirect );

				let lastStats = null;
				for ( const workers of [ 1, 2, 4 ] ) {

					const geometry = base.clone();
					const worker = new NodePLOCWorker( { workers } );
					const { bvh, stats } = await worker.generate( geometry, { indirect, targetLeafSize: 6 } );
					await worker.dispose();

					check( bvh._roots.length === syncBvh._roots.length, `${ label } workers=${ workers }: root count` );
					for ( let r = 0; r < syncBvh._roots.length; r ++ ) {

						check(
							bufferBytes( bvh._roots[ r ] ).equals( bufferBytes( syncBvh._roots[ r ] ) ),
							`${ label } workers=${ workers }: root ${ r } bytes identical`,
						);

					}

					check(
						typedBytes( geometry.index.array ).equals( typedBytes( syncGeometry.index.array ) ),
						`${ label } workers=${ workers }: index bytes identical`,
					);

					const syncIndirect = syncBvh._indirectBuffer;
					const gotIndirect = bvh._indirectBuffer;
					if ( syncIndirect || gotIndirect ) {

						check(
							Boolean( syncIndirect ) === Boolean( gotIndirect ) &&
							typedBytes( gotIndirect ).equals( typedBytes( syncIndirect ) ),
							`${ label } workers=${ workers }: indirect buffer bytes identical`,
						);

					}

					// the parallel stats match the serial reference counts exactly
					check( stats.rounds.join( ',' ) === serialStats.rounds.join( ',' ), `${ label } workers=${ workers }: rounds match serial` );
					check( stats.candidateEvaluations === serialStats.candidateEvaluations, `${ label } workers=${ workers }: candidate evaluations match serial` );
					check( stats.mergedPairs === serialStats.mergedPairs, `${ label } workers=${ workers }: merges match serial` );

					if ( lastStats ) {

						check( stats.rounds.join( ',' ) === lastStats.rounds.join( ',' ), `${ label } workers=${ workers }: rounds stable across worker counts` );

					}

					lastStats = stats;

				}

				// stats invariants on the 4-worker run
				const stats = lastStats;
				check( stats.rounds[ 0 ] === 8192, `${ label }: first round has 8192 clusters` );
				check( stats.rounds[ stats.rounds.length - 1 ] === 1, `${ label }: last round has 1 cluster` );
				let decreasing = true;
				for ( let i = 1; i < stats.rounds.length; i ++ ) {

					if ( stats.rounds[ i ] >= stats.rounds[ i - 1 ] ) decreasing = false;

				}

				check( decreasing, `${ label }: active clusters strictly decrease` );
				check( stats.rounds.length === stats.roundPairs.length + 1, `${ label }: one more round entry than pair lists` );

				let pairSum = 0;
				let pairsValid = true;
				for ( let r = 0; r < stats.roundPairs.length; r ++ ) {

					const pairs = stats.roundPairs[ r ];
					pairSum += pairs.length;
					if ( stats.rounds[ r + 1 ] !== stats.rounds[ r ] - pairs.length ) pairsValid = false;
					const used = new Set();
					for ( const [ p, q ] of pairs ) {

						if ( ! ( p < q ) || used.has( p ) || used.has( q ) ) pairsValid = false;
						used.add( p );
						used.add( q );

					}

				}

				check( pairsValid, `${ label }: round pairs are disjoint and reduce the count` );
				check( pairSum === 8191, `${ label }: 8191 merges for 8192 faces` );
				check( stats.mergedPairs === 8191, `${ label }: mergedPairs total` );

				// per-thread work counters add up and, with 4 workers on 8192 faces, at
				// least two threads each evaluated candidates and merged pairs
				check( stats.threads.length === 4, `${ label }: 4 thread entries` );
				let evalSum = 0;
				let mergeSum = 0;
				let activeThreads = 0;
				for ( const thread of stats.threads ) {

					evalSum += thread.candidateEvaluations;
					mergeSum += thread.mergedPairs;
					if ( thread.candidateEvaluations > 0 && thread.mergedPairs > 0 ) activeThreads ++;

				}

				check( evalSum === stats.candidateEvaluations, `${ label }: thread evaluations sum to total` );
				check( mergeSum === stats.mergedPairs, `${ label }: thread merges sum to total` );
				check( activeThreads >= 2, `${ label }: at least two threads did candidates and merges` );

			}

		},
	},
	{
		group: 'parallel',
		name: 'overlapping generate calls are rejected',
		async run( check ) {

			const worker = new NodePLOCWorker( { workers: 2 } );
			const first = worker.generate( createPreviewGeometry( 16 ) );
			let error = null;
			try {

				await worker.generate( createPreviewGeometry( 8 ) );

			} catch ( e ) {

				error = e;

			}

			check( error !== null && /Already running/.test( error.message ), 'second generate rejects while running' );

			const result = await first;
			check( result.bvh._roots.length === 1, 'first generate completes' );
			check( worker.running === false, 'running flag cleared' );

			const again = await worker.generate( createPreviewGeometry( 8 ) );
			check( again.stats.mergedPairs === 2 * 8 * 8 - 1, 'instance works after completion' );
			await worker.dispose();

		},
	},
	{
		group: 'parallel',
		name: 'a dying worker rejects the build and winds the task down',
		async run( check ) {

			const worker = new NodePLOCWorker( { workers: 2 } );
			const pending = worker.generate( createPreviewGeometry( 64 ) );
			await worker._readyPromise;
			worker._workers[ 0 ].terminate();

			let error = null;
			try {

				await pending;

			} catch ( e ) {

				error = e;

			}

			check( error !== null, 'generate rejects when a worker dies' );
			check( worker.running === false, 'running flag cleared after failure' );
			check( worker.workerCount === 0, 'pool torn down after failure' );

			const recovered = await worker.generate( createPreviewGeometry( 8 ) );
			check( recovered.stats.mergedPairs === 127, 'instance recovers with a fresh pool' );
			await worker.dispose();

		},
	},
	{
		group: 'parallel',
		name: 'dispose rejects in-flight work and leaves no threads behind',
		async run( check ) {

			const worker = new NodePLOCWorker( { workers: 3 } );
			const pending = worker.generate( createPreviewGeometry( 64 ) );
			const disposePromise = worker.dispose();

			let error = null;
			try {

				await pending;

			} catch ( e ) {

				error = e;

			}

			check( error !== null, 'in-flight generate rejects on dispose' );
			await disposePromise;
			check( worker.workerCount === 0, 'no live threads after dispose' );
			check( worker.disposed === true, 'disposed flag set' );

			let secondError = null;
			try {

				await worker.generate( createPreviewGeometry( 8 ) );

			} catch ( e ) {

				secondError = e;

			}

			check( secondError !== null && /disposed/.test( secondError.message ), 'generate after dispose rejects' );

			// dispose is idempotent
			await worker.dispose();
			check( worker.workerCount === 0, 'still no threads after second dispose' );

		},
	},
	{
		group: 'parallel',
		name: 'constructor validates the worker count',
		async run( check ) {

			for ( const bad of [ 0, 17, 2.5, - 1, NaN ] ) {

				let threw = false;
				try {

					new NodePLOCWorker( { workers: bad } );

				} catch {

					threw = true;

				}

				check( threw, `workers=${ bad } is rejected` );

			}

			const one = new NodePLOCWorker( { workers: 1 } );
			check( one.maxWorkerCount === 1, 'workers=1 accepted' );
			await one.dispose();

			const sixteen = new NodePLOCWorker( 16 );
			check( sixteen.maxWorkerCount === 16, 'workers=16 accepted via number shorthand' );
			await sixteen.dispose();

			const defaulted = new NodePLOCWorker();
			check( defaulted.maxWorkerCount >= 1 && defaulted.maxWorkerCount <= 16, 'default worker count in range' );
			await defaulted.dispose();

		},
	},
];

/* -------------------------------------------------------------------------- */
/* mapping cases                                                              */
/* -------------------------------------------------------------------------- */

const mappingCases = [
	{
		group: 'mapping',
		name: 'shuffled face order raycasts match brute force (direct and indirect)',
		run( check ) {

			const rays = makeRays( 101 );
			for ( const indirect of [ false, true ] ) {

				const geometry = makeGridGeometry( 12, 10, { shuffleSeed: 7 } );
				const indexBefore = geometry.index.array.slice();
				const bvh = new MeshBVH( geometry, { strategy: PLOC, indirect } );
				geometry.boundsTree = bvh;

				if ( indirect ) {

					check(
						typedBytes( geometry.index.array ).equals( typedBytes( indexBefore ) ),
						'indirect: input index is not modified',
					);

				} else {

					check(
						! typedBytes( geometry.index.array ).equals( typedBytes( indexBefore ) ),
						'direct: faces are reordered within the root range',
					);

				}

				compareRaySet( check, `shuffled indirect=${ indirect }`, geometry, new MeshBasicMaterial( { side: DoubleSide } ), rays );

			}

		},
	},
	{
		group: 'mapping',
		name: 'group gaps, materials, and per-range reordering',
		run( check ) {

			const geometry = makeGridGeometry( 6, 6 );
			geometry.clearGroups();
			geometry.addGroup( 0, 36, 0 ); // triangles 0..11, material 0
			geometry.addGroup( 72, 72, 1 ); // triangles 24..47, material 1
			// triangles 12..23 and 48..71 are in gaps and excluded from the BVH

			const gapBefore = sortedTriangleIds( geometry, 12, 12 ).join( '|' );
			const range0Before = sortedTriangleIds( geometry, 0, 12 ).join( '|' );
			const range1Before = sortedTriangleIds( geometry, 24, 24 ).join( '|' );

			const bvh = new MeshBVH( geometry, { strategy: PLOC } );
			geometry.boundsTree = bvh;
			check( bvh._roots.length === 2, 'one root per covered group range' );

			// direct reordering stays inside each root range and never touches gaps
			check( sortedTriangleIds( geometry, 0, 12 ).join( '|' ) === range0Before, 'range 0 keeps its triangle set' );
			check( sortedTriangleIds( geometry, 24, 24 ).join( '|' ) === range1Before, 'range 1 keeps its triangle set' );
			check( sortedTriangleIds( geometry, 12, 12 ).join( '|' ) === gapBefore, 'gap triangles are untouched' );

			const materials = [
				new MeshBasicMaterial( { side: FrontSide } ),
				new MeshBasicMaterial( { side: DoubleSide } ),
			];

			// rays from above and below exercise the per-group material sides
			const rays = makeRays( 202 );
			const below = [];
			for ( let i = 0; i < 8; i ++ ) {

				const x = - 1 + 2 * ( ( i * 0.5317 ) % 1 ) + 0.002;
				const y = - 1 + 2 * ( ( i * 0.7137 ) % 1 ) + 0.007;
				below.push( new Ray( new Vector3( x, y, - 3 ), new Vector3( 0, 0, 1 ) ) );

			}

			compareRaySet( check, 'groups above', geometry, materials, rays );
			compareRaySet( check, 'groups below', geometry, materials, below );

		},
	},
	{
		group: 'mapping',
		name: 'drawRange restricts the covered triangles',
		run( check ) {

			const geometry = makeGridGeometry( 10, 8 );
			geometry.setDrawRange( 30, 90 ); // triangles 10..39
			const bvh = new MeshBVH( geometry, { strategy: PLOC } );
			geometry.boundsTree = bvh;
			check( bvh._roots.length === 1, 'single root for the draw range' );
			compareRaySet( check, 'drawRange', geometry, new MeshBasicMaterial( { side: DoubleSide } ), makeRays( 303 ) );

		},
	},
	{
		group: 'mapping',
		name: 'non-indexed geometry works direct, indirect, and after deserialize',
		run( check ) {

			const rays = makeRays( 404 );
			const material = new MeshBasicMaterial( { side: DoubleSide } );

			// direct: an index is created as a side effect
			const directGeometry = makeGridGeometry( 8, 6, { indexed: false } );
			const directBvh = new MeshBVH( directGeometry, { strategy: PLOC } );
			directGeometry.boundsTree = directBvh;
			check( directGeometry.index !== null, 'direct: an index is produced' );
			compareRaySet( check, 'non-indexed direct', directGeometry, material, rays );

			// indirect: no index is ever created
			const indirectGeometry = makeGridGeometry( 8, 6, { indexed: false } );
			const indirectBvh = new MeshBVH( indirectGeometry, { strategy: PLOC, indirect: true } );
			indirectGeometry.boundsTree = indirectBvh;
			check( indirectGeometry.index === null, 'indirect: geometry stays non-indexed' );
			compareRaySet( check, 'non-indexed indirect', indirectGeometry, material, rays );

			// serialization keeps working and can restore onto non-indexed geometry
			const data = MeshBVH.serialize( indirectBvh );
			const restoredGeometry = makeGridGeometry( 8, 6, { indexed: false } );
			const restored = MeshBVH.deserialize( data, restoredGeometry, { setIndex: false } );
			check( restoredGeometry.index === null, 'deserialize with setIndex:false stays non-indexed' );
			for ( let i = 0; i < rays.length; i ++ ) {

				const expected = indirectBvh.raycastFirst( rays[ i ], DoubleSide );
				const got = restored.raycastFirst( rays[ i ], DoubleSide );
				check( ( expected === null ) === ( got === null ), `restored ray ${ i }: presence` );
				if ( expected && got ) {

					check( got.distance === expected.distance && got.faceIndex === expected.faceIndex, `restored ray ${ i }: hit matches` );

				}

			}

		},
	},
	{
		group: 'mapping',
		name: 'refit tracks continuously moving vertices',
		run( check ) {

			const geometry = makeGridGeometry( 8, 8 );
			const bvh = new MeshBVH( geometry, { strategy: PLOC } );
			geometry.boundsTree = bvh;
			const material = new MeshBasicMaterial( { side: DoubleSide } );
			const rays = makeRays( 505, 16 );

			const position = geometry.attributes.position;
			const baseZ = new Float32Array( position.count );
			for ( let i = 0; i < position.count; i ++ ) {

				baseZ[ i ] = position.getZ( i );

			}

			for ( let step = 1; step <= 5; step ++ ) {

				for ( let v = 0; v < position.count; v ++ ) {

					position.setZ( v, baseZ[ v ] + 0.05 * step * Math.sin( v * 1.7 ) );

				}

				position.needsUpdate = true;
				bvh.refit();
				compareRaySet( check, `refit step ${ step }`, geometry, material, rays );

			}

		},
	},
	{
		group: 'mapping',
		name: 'serialize and deserialize preserve the PLOC tree',
		run( check ) {

			const rays = makeRays( 606, 16 );
			for ( const indirect of [ false, true ] ) {

				const geometry = makeGridGeometry( 9, 7, { shuffleSeed: 3 } );
				const bvh = new MeshBVH( geometry, { strategy: PLOC, indirect } );
				const data = MeshBVH.serialize( bvh );
				const restored = MeshBVH.deserialize( data, geometry.clone() );

				for ( let i = 0; i < rays.length; i ++ ) {

					const expected = bvh.raycastFirst( rays[ i ], DoubleSide );
					const got = restored.raycastFirst( rays[ i ], DoubleSide );
					check( ( expected === null ) === ( got === null ), `indirect=${ indirect } ray ${ i }: presence` );
					if ( expected && got ) {

						check(
							got.distance === expected.distance && got.faceIndex === expected.faceIndex,
							`indirect=${ indirect } ray ${ i }: hit matches`,
						);

					}

				}

			}

		},
	},
	{
		group: 'mapping',
		name: 'duplicate centroids still hit every duplicated face',
		run( check ) {

			// ten distinct triangles, the first four duplicated three times each
			const rand = mulberry32( 909 );
			const triangles = [];
			for ( let i = 0; i < 10; i ++ ) {

				const cx = rand() * 2 - 1;
				const cy = rand() * 2 - 1;
				const s = 0.2 + rand() * 0.2;
				triangles.push( [
					cx, cy, 0.1 * i,
					cx + s, cy, 0.1 * i,
					cx, cy + s, 0.1 * i,
				] );

			}

			const all = triangles.slice();
			for ( let t = 0; t < 4; t ++ ) {

				for ( let k = 0; k < 2; k ++ ) {

					all.push( triangles[ t ].slice() );

				}

			}

			// shuffle the triangle order before building
			const order = [ ...Array( all.length ).keys() ];
			for ( let i = order.length - 1; i > 0; i -- ) {

				const j = Math.floor( rand() * ( i + 1 ) );
				const t = order[ i ];
				order[ i ] = order[ j ];
				order[ j ] = t;

			}

			const vertices = [];
			for ( const index of order ) {

				vertices.push( ...all[ index ] );

			}

			const geometry = new BufferGeometry();
			geometry.setAttribute( 'position', new Float32BufferAttribute( vertices, 3 ) );
			geometry.setIndex( Array.from( { length: all.length * 3 }, ( _, i ) => i ) );
			const bvh = new MeshBVH( geometry, { strategy: PLOC } );
			geometry.boundsTree = bvh;
			const material = new MeshBasicMaterial( { side: DoubleSide } );

			const rays = [
				new Ray( new Vector3( triangles[ 0 ][ 0 ] + 0.05, triangles[ 0 ][ 1 ] + 0.05, 3 ), new Vector3( 0, 0, - 1 ) ),
				new Ray( new Vector3( triangles[ 1 ][ 0 ] + 0.05, triangles[ 1 ][ 1 ] + 0.05, 3 ), new Vector3( 0, 0, - 1 ) ),
				new Ray( new Vector3( triangles[ 2 ][ 0 ] + 0.05, triangles[ 2 ][ 1 ] + 0.05, 3 ), new Vector3( 0, 0, - 1 ) ),
			];

			for ( let i = 0; i < rays.length; i ++ ) {

				const refHits = bruteForceHits( geometry, material, rays[ i ] );
				const bvhHits = bvh.raycast( rays[ i ], DoubleSide );
				check( bvhHits.length === refHits.length, `duplicates ray ${ i }: all ${ refHits.length } hits found` );
				if ( refHits.length ) {

					check(
						Math.abs( bvhHits[ 0 ].distance - refHits[ 0 ].distance ) < 1e-6 ||
						bvhHits.some( h => Math.abs( h.distance - refHits[ 0 ].distance ) < 1e-6 ),
						`duplicates ray ${ i }: closest distance matches`,
					);

				}

			}

		},
	},
];

/* -------------------------------------------------------------------------- */
/* render cases                                                               */
/* -------------------------------------------------------------------------- */

// independent re-derivation of the example preview: same camera and shading math,
// but rendered through a synchronously built PLOC tree
function renderPreviewIndependently( width, height ) {

	const geometry = createPreviewGeometry( 64 );
	const bvh = new MeshBVH( geometry, { strategy: PLOC, targetLeafSize: 6 } );

	const origin = new Vector3( 0, - 2.2, 1.35 );
	const lookAt = new Vector3( 0, 0, 0.05 );
	const forward = lookAt.clone().sub( origin ).normalize();
	const right = new Vector3().crossVectors( forward, new Vector3( 0, 0, 1 ) ).normalize();
	const up = new Vector3().crossVectors( right, forward ).normalize();
	const halfH = Math.tan( Math.PI / 3.4 / 2 );
	const halfW = halfH * ( width / height );
	const light = new Vector3( 0.4, - 0.6, 1 ).normalize();

	const data = new Uint8Array( width * height * 3 );
	for ( let y = 0; y < height; y ++ ) {

		for ( let x = 0; x < width; x ++ ) {

			const ndcX = ( ( x + 0.5 ) / width ) * 2 - 1;
			const ndcY = 1 - ( ( y + 0.5 ) / height ) * 2;
			const direction = forward.clone()
				.addScaledVector( right, ndcX * halfW )
				.addScaledVector( up, ndcY * halfH )
				.normalize();
			const ray = new Ray( origin.clone(), direction );
			const hit = bvh.raycastFirst( ray, DoubleSide );
			const offset = ( y * width + x ) * 3;
			if ( hit ) {

				const normal = hit.face.normal;
				const sign = normal.dot( direction ) > 0 ? - 1 : 1;
				const diffuse = Math.max( 0, sign * normal.dot( light ) );
				const shade = 0.2 + 0.8 * diffuse;
				const t = Math.min( 1, Math.max( 0, hit.point.z * 1.6 + 0.5 ) );
				data[ offset + 0 ] = Math.round( 255 * shade * ( 0.25 + 0.55 * t ) );
				data[ offset + 1 ] = Math.round( 255 * shade * ( 0.45 + 0.25 * t ) );
				data[ offset + 2 ] = Math.round( 255 * shade * ( 0.85 - 0.45 * t ) );

			} else {

				const g = Math.round( 30 + 60 * ( y / height ) );
				data[ offset + 0 ] = g;
				data[ offset + 1 ] = g;
				data[ offset + 2 ] = Math.min( 255, g + 30 );

			}

		}

	}

	return data;

}

const renderCases = [
	{
		group: 'render',
		name: 'worker-built preview is deterministic and matches an independent render',
		async run( check ) {

			const width = 48;
			const height = 36;
			const first = await renderPreview( { width, height, workers: 2 } );
			const second = await renderPreview( { width, height, workers: 2 } );

			check( first.data.length === width * height * 3, 'pixel buffer size' );
			check( typedBytes( first.data ).equals( typedBytes( second.data ) ), 'render is deterministic' );
			check( first.stats.rounds[ 0 ] === 8192, 'render builds the 8192-face grid' );
			check( first.stats.mergedPairs === 8191, 'render merges 8191 pairs' );
			check( first.stats.threads.length === 2, 'render used 2 threads' );

			// non-trivial image content
			let min = 255;
			let max = 0;
			for ( const v of first.data ) {

				if ( v < min ) min = v;
				if ( v > max ) max = v;

			}

			check( max - min > 40, 'image has real contrast' );

			// every pixel matches an independent render through a synchronous PLOC tree
			const expected = renderPreviewIndependently( width, height );
			check( typedBytes( first.data ).equals( typedBytes( expected ) ), 'pixels match the independent render' );

		},
	},
	{
		group: 'render',
		name: 'example writes a valid PPM preview',
		async run( check ) {

			const outPath = path.join( fs.mkdtempSync( path.join( os.tmpdir(), 'ploc-example-' ) ), 'ploc.ppm' );
			execFileSync( process.execPath, [ 'example/ploc.mjs', outPath ], { cwd: REPO_ROOT, stdio: 'pipe' } );

			const buffer = fs.readFileSync( outPath );
			const header = 'P6\n160 120\n255\n';
			check( buffer.length > 64, 'ppm is not empty' );
			check( buffer.subarray( 0, header.length ).toString( 'ascii' ) === header, 'ppm header' );
			check( buffer.length === header.length + 160 * 120 * 3, 'ppm pixel byte count' );

			// the file content matches an in-process render with the same defaults
			const expected = await renderPreview();
			check(
				buffer.subarray( header.length ).equals( typedBytes( expected.data ) ),
				'ppm pixels match the in-process render',
			);

			// the exported encoder produces the same bytes
			const encoded = encodePPM( expected.width, expected.height, expected.data );
			check( encoded.equals( buffer ), 'encodePPM output matches the example file' );

			fs.rmSync( path.dirname( outPath ), { recursive: true, force: true } );

		},
	},
];

export const cases = [
	...clusteringCases,
	...parallelCases,
	...mappingCases,
	...renderCases,
];
