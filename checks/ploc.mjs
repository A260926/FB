import assert from 'node:assert/strict';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { execFile } from 'node:child_process';
import { promisify } from 'node:util';
import {
	BufferGeometry,
	Float32BufferAttribute,
	Mesh,
	MeshBasicMaterial,
	Raycaster,
	Vector3,
	DoubleSide,
} from 'three';
import { MeshBVH, PLOC } from '../src/index.js';
import { NodePLOCWorker } from '../src/workers/NodePLOCWorker.js';
import {
	clusterPLOCSerial,
	packPLOCHierarchy,
} from '../src/core/build/PLOCBuilder.js';

const execFileAsync = promisify( execFile );

/**
 * @typedef {Object} Case
 * @property {'clustering'|'parallel'|'mapping'|'render'} group
 * @property {string} name
 * @property {(check:(value:boolean,message?:string)=>void)=>void|Promise<void>} run
 */

function checkThrows( check, fn, pattern, message ) {

	let threw = null;
	try {

		const result = fn();
		if ( result && typeof result.then === 'function' ) {

			throw new Error( 'checkThrows received a Promise; use checkRejects for async code.' );

		}

	} catch ( error ) {

		threw = error;

	}

	check( threw !== null, `${ message }: expected a throw` );
	if ( pattern ) check( pattern.test( threw?.message || '' ), `${ message }: ${ threw?.message }` );

}

async function checkRejects( check, promiseFactory, pattern, message ) {

	let error = null;
	try {

		await promiseFactory();

	} catch ( caught ) {

		error = caught;

	}

	check( error !== null, `${ message }: expected rejection` );
	if ( pattern ) check( pattern.test( error?.message || '' ), `${ message }: ${ error?.message }` );

}

function mulberry32( seed ) {

	return function random() {

		let t = seed += 0x6D2B79F5;
		t = Math.imul( t ^ ( t >>> 15 ), t | 1 );
		t ^= t + Math.imul( t ^ ( t >>> 7 ), t | 61 );
		return ( ( t ^ ( t >>> 14 ) ) >>> 0 ) / 4294967296;

	};

}

function shuffle( array, random ) {

	for ( let i = array.length - 1; i > 0; i -- ) {

		const j = Math.floor( random() * ( i + 1 ) );
		[ array[ i ], array[ j ] ] = [ array[ j ], array[ i ] ];

	}

	return array;

}

function flattenBoxes( boxes ) {

	const bounds = new Float64Array( boxes.length * 6 );
	const original = new Uint32Array( boxes.length );
	boxes.forEach( ( box, i ) => {

		bounds.set( [ ...box.min, ...box.max ], i * 6 );
		original[ i ] = box.original ?? i;

	} );
	return { bounds, original };

}

// An intentionally independent, small serial implementation of the specification.
function referenceMortonSpread( value ) {

	let x = value & 1023;
	x = ( x | ( x << 16 ) ) & 0x30000ff;
	x = ( x | ( x << 8 ) ) & 0x300f00f;
	x = ( x | ( x << 4 ) ) & 0x30c30c3;
	x = ( x | ( x << 2 ) ) & 0x9249249;
	return x;

}

function referenceArea( a, b ) {

	const dx = Math.max( a.max[ 0 ], b.max[ 0 ] ) - Math.min( a.min[ 0 ], b.min[ 0 ] );
	const dy = Math.max( a.max[ 1 ], b.max[ 1 ] ) - Math.min( a.min[ 1 ], b.min[ 1 ] );
	const dz = Math.max( a.max[ 2 ], b.max[ 2 ] ) - Math.min( a.min[ 2 ], b.min[ 2 ] );
	return 2 * ( dx * dy + dx * dz + dy * dz );

}

function referenceLeafOrder( cluster, out = [] ) {

	if ( cluster.local >= 0 ) out.push( cluster.local );
	else referenceLeafOrder( cluster.left, out ), referenceLeafOrder( cluster.right, out );
	return out;

}

function referenceCluster( boxes ) {

	const n = boxes.length;
	if ( n === 0 ) return { rounds: [], root: null, order: [] };

	const centers = boxes.map( b => [
		( b.min[ 0 ] + b.max[ 0 ] ) / 2,
		( b.min[ 1 ] + b.max[ 1 ] ) / 2,
		( b.min[ 2 ] + b.max[ 2 ] ) / 2,
	] );
	const cmin = [ Infinity, Infinity, Infinity ];
	const cmax = [ - Infinity, - Infinity, - Infinity ];
	for ( const c of centers ) for ( let a = 0; a < 3; a ++ ) {

		cmin[ a ] = Math.min( cmin[ a ], c[ a ] );
		cmax[ a ] = Math.max( cmax[ a ], c[ a ] );

	}

	const codes = centers.map( ( c, i ) => {

		const q = c.map( ( value, a ) => {

			const span = cmax[ a ] - cmin[ a ];
			if ( span === 0 ) return 0;
			return Math.min( 1023, Math.floor( 1024 * ( ( value - cmin[ a ] ) / span ) ) );

		} );
		return referenceMortonSpread( q[ 0 ] ) |
			( referenceMortonSpread( q[ 1 ] ) << 1 ) |
			( referenceMortonSpread( q[ 2 ] ) << 2 ) + 0;

	} );

	const order = Array.from( { length: n }, ( _, i ) => i )
		.sort( ( a, b ) => codes[ a ] - codes[ b ] || ( boxes[ a ].original ?? a ) - ( boxes[ b ].original ?? b ) || a - b );

	let active = order.map( local => ( {
		local,
		minOriginal: boxes[ local ].original ?? local,
		box: boxes[ local ],
		left: { local },
		right: null,
	} ) );

	const rounds = [];
	while ( active.length > 1 ) {

		const m = active.length;
		const best = new Array( m ).fill( - 1 );
		for ( let i = 0; i < m; i ++ ) {

			let bestArea = Infinity;
			let bestMin = Infinity;
			const lo = Math.max( 0, i - 16 );
			const hi = Math.min( m - 1, i + 16 );
			for ( let j = lo; j <= hi; j ++ ) {

				if ( i === j ) continue;
				const area = referenceArea( active[ i ].box, active[ j ].box );
				const minOriginal = active[ j ].minOriginal;
				if ( area < bestArea || ( area === bestArea && minOriginal < bestMin ) ) {

					bestArea = area;
					bestMin = minOriginal;
					best[ i ] = j;

				}

			}

		}

		const pairs = [];
		const used = new Set();
		for ( let i = 0; i < m; i ++ ) {

			const j = best[ i ];
			if ( j > i && best[ j ] === i && ! used.has( i ) && ! used.has( j ) ) {

				pairs.push( [ i, j ] );
				used.add( i );
				used.add( j );

			}

		}

		const parents = new Map();
		const removed = new Set();
		for ( const [ i, j ] of pairs ) {

			const a = active[ i ];
			const b = active[ j ];
			const box = {
				min: [
					Math.min( a.box.min[ 0 ], b.box.min[ 0 ] ),
					Math.min( a.box.min[ 1 ], b.box.min[ 1 ] ),
					Math.min( a.box.min[ 2 ], b.box.min[ 2 ] ),
				],
				max: [
					Math.max( a.box.max[ 0 ], b.box.max[ 0 ] ),
					Math.max( a.box.max[ 1 ], b.box.max[ 1 ] ),
					Math.max( a.box.max[ 2 ], b.box.max[ 2 ] ),
				],
			};
			parents.set( i, {
				local: - 1,
				minOriginal: Math.min( a.minOriginal, b.minOriginal ),
				box,
				left: a,
				right: b,
			} );
			removed.add( j );

		}

		const next = [];
		for ( let i = 0; i < m; i ++ ) {

			if ( parents.has( i ) ) next.push( parents.get( i ) );
			else if ( ! removed.has( i ) ) next.push( active[ i ] );

		}

		rounds.push( { active: m, pairs: pairs.map( ( [ i, j ] ) => [ i, j ].sort( ( a, b ) => a - b ) ) } );
		active = next;

	}

	return { rounds, root: active[ 0 ] || null, order };

}

function parsePackedRoot( buffer ) {

	const f32 = new Float32Array( buffer );
	const u32 = new Uint32Array( buffer );
	const u16 = new Uint16Array( buffer );
	assert.equal( buffer.byteLength % 32, 0 );

	function walk( node ) {

		const node16 = node * 16;
		const node32 = node * 8;
		const isLeaf = u16[ node16 + 15 ] === 0xffff;
		const bounds = Array.from( f32.subarray( node32, node32 + 6 ) );
		if ( isLeaf ) {

			return {
				node,
				isLeaf,
				bounds,
				offset: u32[ node32 + 6 ],
				count: u16[ node16 + 14 ],
				depth: 0,
			};

		}

		const relative = u32[ node32 + 6 ];
		const leftNode = node + 1;
		const rightNode = node + relative;
		assert.ok( relative >= 2, 'right child must follow left subtree' );
		assert.ok( rightNode * 32 < buffer.byteLength, 'right child is inside buffer' );
		const left = walk( leftNode );
		const right = walk( rightNode );
		return { node, isLeaf, bounds, left, right, rightNode, depth: 1 + Math.max( left.depth, right.depth ) };

	}

	return walk( 0 );

}

function packedLeavesInDFS( node, out = [] ) {

	if ( node.isLeaf ) out.push( node );
	else packedLeavesInDFS( node.left, out ), packedLeavesInDFS( node.right, out );
	return out;

}

function packedSubtreeCount( node ) {

	if ( node.isLeaf ) return node.count;
	return packedSubtreeCount( node.left ) + packedSubtreeCount( node.right );

}

function expectReferenceMatch( check, boxes, label ) {

	const reference = referenceCluster( boxes );
	const actual = clusterPLOCSerial( flattenBoxes( boxes ) );
	check( actual.rounds.length === reference.rounds.length, `${ label }: round count` );
	for ( let r = 0; r < Math.min( actual.rounds.length, reference.rounds.length ); r ++ ) {

		const ar = actual.rounds[ r ];
		const rr = reference.rounds[ r ];
		check( ar.activeClusters === rr.active, `${ label } round ${ r } active count` );
		const actualPairs = ar.mutualPairs
			.map( p => [ p.early, p.late ].sort( ( a, b ) => a - b ).join( ':' ) )
			.sort();
		const expectedPairs = rr.pairs.map( p => p.join( ':' ) ).sort();
		check( JSON.stringify( actualPairs ) === JSON.stringify( expectedPairs ), `${ label } round ${ r } mutual pairs` );

	}

	const expectedOrder = referenceLeafOrder( reference.root );
	const actualOrder = [];
	( function collect( cluster ) {

		if ( ! cluster ) return;
		if ( cluster.faceLocal >= 0 ) actualOrder.push( cluster.faceLocal );
		else collect( cluster.left ), collect( cluster.right );

	} )( actual.root );
	check( JSON.stringify( actualOrder ) === JSON.stringify( expectedOrder ), `${ label } DFS tree order` );

	const packed = packPLOCHierarchy( {
		root: actual.root,
		faceCount: boxes.length,
		targetLeafSize: 1,
		maxDepth: 1000,
	} );
	check( packed.permutation.length === boxes.length, `${ label } permutation length` );
	check( new Set( packed.permutation ).size === boxes.length, `${ label } permutation is complete` );
	const parsed = parsePackedRoot( packed.buffer );
	const leaves = packedLeavesInDFS( parsed );
	let expectedOffset = 0;
	for ( const leaf of leaves ) {

		check( leaf.count === 1, `${ label } full tree packs one-face leaves` );
		check( leaf.offset === expectedOffset, `${ label } leaf offset is DFS-contiguous` );
		expectedOffset += leaf.count;

	}

	check( expectedOffset === boxes.length, `${ label } leaf intervals cover all faces` );

}

function smallScenarios() {

	return {
		sameCode: [
			{ min: [ - 1, - 1, 0 ], max: [ 1, 1, 0 ] },
			{ min: [ - .2, - 2, 0 ], max: [ .2, 2, 0 ] },
			{ min: [ - 2, - .2, 0 ], max: [ 2, .2, 0 ] },
			{ min: [ - .5, - .5, - 1 ], max: [ .5, .5, 1 ] },
			{ min: [ - .1, - 3, 0 ], max: [ .1, 3, 0 ] },
			{ min: [ - 1.4, - .15, 0 ], max: [ - .2, .15, 0 ] },
		],
		planar: [
			{ min: [ 0, 0, 0 ], max: [ 1, 1, 0 ] },
			{ min: [ 2, .2, 0 ], max: [ 3.1, 1.3, 0 ] },
			{ min: [ .4, 2, 0 ], max: [ 1.5, 3.2, 0 ] },
			{ min: [ 3, 2.4, 0 ], max: [ 4, 3.5, 0 ] },
			{ min: [ 1.4, .8, 0 ], max: [ 2.2, 1.7, 0 ] },
			{ min: [ 5, 1, 0 ], max: [ 5.8, 2, 0 ] },
			{ min: [ 2, 4, 0 ], max: [ 3, 5, 0 ] },
		],
		single: [ { min: [ - 2, - 3, - 4 ], max: [ 5, 7, 9 ] } ],
		asymmetric: [
			{ min: [ 0, 0, 0 ], max: [ 10, .2, .2 ] },
			{ min: [ 1, 1, 0 ], max: [ 1.4, 1.4, .4 ] },
			{ min: [ 1.6, 1.1, 0 ], max: [ 2, 1.5, .4 ] },
			{ min: [ 8, 8, 0 ], max: [ 8.5, 8.5, 2 ] },
			{ min: [ - 4, 2, - 1 ], max: [ - 1, 2.4, 1 ] },
			{ min: [ 3, - 3, 0 ], max: [ 7, - 2.5, .5 ] },
			{ min: [ 4, - 2.2, 0 ], max: [ 4.4, 1, .4 ] },
			{ min: [ 9, - 1, 0 ], max: [ 9.3, 3, .3 ] },
		],
	};

}

function makeGridGeometry( triangles = 8192 ) {

	const positions = new Float32Array( triangles * 9 );
	let p = 0;
	const columns = 128;
	for ( let i = 0; i < triangles; i ++ ) {

		const x = ( i % columns ) * 1.07;
		const y = Math.floor( i / columns ) * 1.13;
		positions[ p ++ ] = x; positions[ p ++ ] = y; positions[ p ++ ] = 0;
		positions[ p ++ ] = x + .91; positions[ p ++ ] = y + .04; positions[ p ++ ] = 0;
		positions[ p ++ ] = x + .03; positions[ p ++ ] = y + .88; positions[ p ++ ] = 0;

	}

	const geometry = new BufferGeometry();
	geometry.setAttribute( 'position', new Float32BufferAttribute( positions, 3 ) );
	return geometry;

}

function bytesEqual( a, b ) {

	return Buffer.from( a ).equals( Buffer.from( b ) );

}

function expectBVHBytes( check, a, b, label, { indirect = false } = {} ) {

	check( a._roots.length === b._roots.length, `${ label } root count` );
	for ( let i = 0; i < a._roots.length; i ++ ) {

		check( bytesEqual( a._roots[ i ], b._roots[ i ] ), `${ label } root ${ i } bytes` );

	}

	if ( indirect ) {

		check( a._indirectBuffer !== null && b._indirectBuffer !== null, `${ label } indirect buffers exist` );
		check(
			a._indirectBuffer.constructor === b._indirectBuffer.constructor &&
			bytesEqual( a._indirectBuffer.buffer, b._indirectBuffer.buffer ),
			`${ label } indirectBuffer bytes`,
		);

	} else {

		check( a.geometry.index !== null && b.geometry.index !== null, `${ label } indexes exist` );
		check(
			a.geometry.index.array.constructor === b.geometry.index.array.constructor &&
			bytesEqual( a.geometry.index.array.buffer, b.geometry.index.array.buffer ),
			`${ label } index bytes`,
		);

	}

}

function makeMappedGeometry() {

	const cols = 8;
	const rows = 6;
	const rawFaces = [];
	let id = 0;
	for ( let row = 0; row < rows; row ++ ) {

		for ( let col = 0; col < cols; col ++ ) {

			const x = col * 1.2;
			const y = row * 1.2;
			const verts = [
				[ x, y, 0 ],
				[ x + 1, y + .04, 0 ],
				[ x + .05, y + .95, 0 ],
				[ x + 1, y + .04, 0 ],
				[ x + 1.04, y + 1, 0 ],
				[ x + .05, y + .95, 0 ],
			];
			for ( const tri of [ verts.slice( 0, 3 ), verts.slice( 3, 6 ) ] ) {

				const u0 = ( id * .137 ) % .7;
				const v0 = ( id * .271 ) % .7;
				rawFaces.push( {
					id: id ++,
					vertices: tri,
					uvs: [ [ u0, v0 ], [ u0 + .27, v0 + .03 ], [ u0 + .04, v0 + .31 ] ],
					centroid: {
						x: ( tri[ 0 ][ 0 ] + tri[ 1 ][ 0 ] + tri[ 2 ][ 0 ] ) / 3,
						y: ( tri[ 0 ][ 1 ] + tri[ 1 ][ 1 ] + tri[ 2 ][ 1 ] ) / 3,
					},
				} );

			}

		}

	}

	const random = mulberry32( 31337 );
	const faces = shuffle( rawFaces, random );
	const positionArray = new Float32Array( faces.length * 9 );
	const uvArray = new Float32Array( faces.length * 6 );
	faces.forEach( ( face, faceIndex ) => {

		face.vertices.flat().forEach( ( value, i ) => positionArray[ faceIndex * 9 + i ] = value );
		face.uvs.flat().forEach( ( value, i ) => uvArray[ faceIndex * 6 + i ] = value );

	} );

	const geometry = new BufferGeometry();
	geometry.setAttribute( 'position', new Float32BufferAttribute( positionArray, 3 ) );
	geometry.setAttribute( 'uv', new Float32BufferAttribute( uvArray, 2 ) );

	// Faces 0..39 are group 0, faces 48..87 are group 1. Faces 40..47 and 88..95
	// are uncovered gaps, exercising the existing group-range coverage rules.
	geometry.addGroup( 0, 40 * 3, 0 );
	geometry.addGroup( 48 * 3, 40 * 3, 1 );
	return { geometry, faces, includedFaces: [ ...Array.from( { length: 40 }, ( _, i ) => i ), ...Array.from( { length: 40 }, ( _, i ) => i + 48 ) ] };

}

async function compareMappedRays( check, label, indirect, { refitSteps = 0, serialize = false } = {} ) {

	const source = makeMappedGeometry();
	let geometry = source.geometry;
	const bvh = new MeshBVH( geometry, { strategy: PLOC, indirect, targetLeafSize: 4, verbose: false } );
	const materials = [
		new MeshBasicMaterial( { side: DoubleSide } ),
		new MeshBasicMaterial( { color: 0x88aaff, side: DoubleSide } ),
	];
	let queryBVH = bvh;
	let queryGeometry = geometry;
	const mesh = new Mesh( geometry, materials );
	mesh.updateMatrixWorld( true );
	const raycaster = new Raycaster();

	for ( let step = 1; step <= refitSteps; step ++ ) {

		const position = geometry.attributes.position;
		for ( let i = 0; i < position.count; i ++ ) {

			const x = position.getX( i );
			const y = position.getY( i );
			position.setZ( i, .16 * step + .13 * Math.sin( x * 1.4 + step ) * Math.cos( y * 1.1 - step * .4 ) );

		}

		position.needsUpdate = true;
		bvh.refit();
		geometry.boundingBox = null;
		geometry.boundingSphere = null;

	}

	if ( serialize ) {

		const data = MeshBVH.serialize( bvh, { cloneBuffers: true } );
		queryGeometry = geometry.clone();
		queryBVH = MeshBVH.deserialize( data, queryGeometry, { setIndex: false } );
		geometry = queryGeometry;
		mesh.geometry = queryGeometry;
		queryGeometry.boundingBox = null;
		queryGeometry.boundingSphere = null;

	}

	let comparisons = 0;
	const sampleFaces = source.includedFaces.filter( ( _, i ) => i % 2 === 0 );
	for ( const faceIndex of sampleFaces ) {

		const center = source.faces[ faceIndex ].centroid;
		const zOrigin = refitSteps ? 4 : 3;
		raycaster.set( new Vector3( center.x, center.y, zOrigin ), new Vector3( 0, 0, - 1 ) );
		const expected = raycaster.intersectObject( mesh, false )[ 0 ] || null;
		const actual = queryBVH.raycastFirst( raycaster.ray, materials );
		check( expected !== null && actual !== null, `${ label } hit for shuffled/grouped face ${ faceIndex }` );
		if ( ! expected || ! actual ) continue;

		check( Math.abs( actual.distance - expected.distance ) < 2e-5, `${ label } distance ${ faceIndex }: ${ actual.distance } vs ${ expected.distance }` );
		check( actual.faceIndex === expected.faceIndex, `${ label } faceIndex ${ faceIndex }: ${ actual.faceIndex } vs ${ expected.faceIndex }` );
		check( actual.uv && expected.uv && actual.uv.distanceTo( expected.uv ) < 2e-5, `${ label } UV ${ faceIndex }` );
		check( actual.face?.materialIndex === expected.face?.materialIndex, `${ label } material ${ faceIndex }` );
		comparisons ++;

	}

	check( comparisons >= 30, `${ label } performed real ray comparisons` );
	return { bvh: queryBVH, geometry: queryGeometry };

}

export const cases = [
	{
		group: 'clustering',
		name: 'serial pairing and DFS tree match an independent reference, including degenerate layouts',
		run( check ) {

			const scenarios = smallScenarios();
			for ( const [ name, boxes ] of Object.entries( scenarios ) ) {

				expectReferenceMatch( check, boxes, name );

			}

			check( true, 'clustering scenarios executed' );

		},
	},
	{
		group: 'clustering',
		name: 'equal Morton centroids use original face order as the stable tie breaker',
		run( check ) {

			const boxes = Array.from( { length: 9 }, ( _, i ) => ( {
				original: 100 - i * 7,
				min: [ - .5 - i * .01, - .5, 0 ],
				max: [ .5 + i * .01, .5, 0 ],
			} ) );
			const { original, bounds } = flattenBoxes( boxes );
			const result = clusterPLOCSerial( { original, bounds } );
			check( result.order.every( ( value, i ) => i === 0 || original[ result.order[ i - 1 ] ] <= original[ value ] ), 'equal codes sorted by original face order' );
			check( result.rounds.length > 0, 'equal-code clusters still merge' );
			check( result.root.count === 9, 'all equal-code faces reach one root' );

		},
	},
	{
		group: 'clustering',
		name: 'targetLeafSize and maxDepth fold complete subtrees into contiguous leaves',
		run( check ) {

			const random = mulberry32( 4242 );
			const boxes = Array.from( { length: 43 }, () => {

				const x = random() * 20;
				const y = random() * 20;
				const z = random() * 4;
				const s = .25 + random() * 1.4;
				return { min: [ x, y, z ], max: [ x + s, y + s * .8, z + s ] };

			} );
			const serial = clusterPLOCSerial( flattenBoxes( boxes ) );

			const targetPacked = packPLOCHierarchy( {
				root: serial.root,
				faceCount: boxes.length,
				targetLeafSize: 5,
				maxDepth: 100,
			} );
			const targetTree = parsePackedRoot( targetPacked.buffer );
			for ( const leaf of packedLeavesInDFS( targetTree ) ) {

				check( leaf.count <= 5, 'targetLeafSize folds subtree into leaf of at most target size' );

			}

			const depthPacked = packPLOCHierarchy( {
				root: serial.root,
				faceCount: boxes.length,
				targetLeafSize: 1,
				maxDepth: 3,
			} );
			const depthTree = parsePackedRoot( depthPacked.buffer );
			check( depthTree.depth <= 3, 'maxDepth limits packed node depth' );
			for ( const leaf of packedLeavesInDFS( depthTree ) ) {

				check( leaf.depth <= 3, 'truncated leaves respect maxDepth' );

			}

			const leaves = packedLeavesInDFS( depthTree );
			let offset = 0;
			for ( const leaf of leaves ) {

				check( leaf.offset === offset, 'truncated leaf range is contiguous' );
				offset += leaf.count;

			}

			check( offset === boxes.length, 'truncated leaves cover complete subtree ranges' );

		},
	},
	{
		group: 'clustering',
		name: 'single-face and empty geometries produce queryable native roots',
		async run( check ) {

			const single = new BufferGeometry();
			single.setAttribute( 'position', new Float32BufferAttribute( [ 0, 0, 0, 1, 0, 0, 0, 1, 0 ], 3 ) );
			const singleBVH = new MeshBVH( single, { strategy: PLOC, targetLeafSize: 1, verbose: false } );
			const missRaycaster = new Raycaster();
			missRaycaster.set( new Vector3( 5, 5, 2 ), new Vector3( 0, 0, - 1 ) );
			check( singleBVH._roots.length === 1 && singleBVH._roots[ 0 ].byteLength === 32, 'one face has one native leaf root' );
			check( singleBVH.raycastFirst( missRaycaster.ray, DoubleSide ) === null, 'miss ray returns null' );

			const empty = new BufferGeometry();
			empty.setAttribute( 'position', new Float32BufferAttribute( [], 3 ) );
			const emptyBVH = new MeshBVH( empty, { strategy: PLOC, verbose: false } );
			check( emptyBVH._roots.length === 1 && emptyBVH._roots[ 0 ].byteLength === 32, 'empty geometry has one packed empty root' );
			check( emptyBVH.raycastFirst( new Raycaster().ray, DoubleSide ) === null, 'empty PLOC tree is queryable' );
			let shapecastCalls = 0;
			emptyBVH.shapecast( {
				intersectsBounds: () => true,
				intersectsRange: () => {

					shapecastCalls ++;
					return false;

				},
			} );
			check( shapecastCalls <= 1, 'empty shapecast does not iterate phantom primitives' );

			const attributeLess = new BufferGeometry();
			const attributeLessBVH = new MeshBVH( attributeLess, { strategy: PLOC, verbose: false } );
			check( attributeLessBVH.raycastFirst( new Raycaster().ray, DoubleSide ) === null, 'attribute-less empty geometry builds queryable PLOC tree' );

		},
	},
	{
		group: 'parallel',
		name: 'sync and Node workers 1/2/4 produce byte-identical direct packed trees',
		async run( check ) {

			for ( const workers of [ 1, 2, 4 ] ) {

				const syncGeometry = makeGridGeometry( 1024 );
				const workerGeometry = makeGridGeometry( 1024 );
				const syncBVH = new MeshBVH( syncGeometry, { strategy: PLOC, targetLeafSize: 3, verbose: false } );
				const worker = new NodePLOCWorker( { workers } );
				try {

					const result = await worker.generate( workerGeometry, { targetLeafSize: 3, verbose: false } );
					check( result.bvh instanceof MeshBVH, `workers=${ workers } resolves a MeshBVH` );
					expectBVHBytes( check, syncBVH, result.bvh, `direct workers=${ workers }`, { indirect: false } );
					check( result.bvh.roots === result.bvh._roots, `workers=${ workers } exposes serialized roots view` );

				} finally {

					await worker.dispose();

				}

			}

		},
	},
	{
		group: 'parallel',
		name: 'sync and Node workers 1/2/4 produce byte-identical indirect buffers',
		async run( check ) {

			for ( const workers of [ 1, 2, 4 ] ) {

				const syncGeometry = makeGridGeometry( 777 );
				const workerGeometry = makeGridGeometry( 777 );
				const syncBVH = new MeshBVH( syncGeometry, { strategy: PLOC, indirect: true, targetLeafSize: 5, verbose: false } );
				check( syncGeometry.index === null, 'indirect construction does not create an input index' );
				const worker = new NodePLOCWorker( workers );
				try {

					const result = await worker.generate( workerGeometry, { indirect: true, targetLeafSize: 5, verbose: false } );
					check( workerGeometry.index === null, `workers=${ workers } leaves indirect input index untouched` );
					expectBVHBytes( check, syncBVH, result.bvh, `indirect workers=${ workers }`, { indirect: true } );

				} finally {

					await worker.dispose();

				}

			}

		},
	},
	{
		group: 'parallel',
		name: '8192-face four-thread build records real candidate and merge work by threadId',
		async run( check ) {

			const syncGeometry = makeGridGeometry( 8192 );
			const workerGeometry = makeGridGeometry( 8192 );
			const syncBVH = new MeshBVH( syncGeometry, { strategy: PLOC, targetLeafSize: 2, verbose: false } );
			const worker = new NodePLOCWorker( { workers: 4 } );
			try {

				const { bvh, stats } = await worker.generate( workerGeometry, { targetLeafSize: 2, verbose: false } );
				expectBVHBytes( check, syncBVH, bvh, '8192 four-worker', { indirect: false } );
				check( Array.isArray( stats.activeClustersPerRound ), 'stats include activeClustersPerRound' );
				check( stats.activeClustersPerRound[ 0 ] === 8192, 'first round has all active clusters' );
				check( stats.activeClustersPerRound.at( - 1 ) === 2, 'last recorded round merges two clusters' );
				check(
					stats.activeClustersPerRound.every( ( value, i ) => i === 0 || value < stats.activeClustersPerRound[ i - 1 ] ),
					'active cluster counts decrease each round',
				);

				const threadRows = Object.values( stats.threads || stats.threadStats || {} );
				const activeThreads = threadRows.filter( row => row.candidateEvaluations > 0 && row.mergedPairs > 0 );
				check( threadRows.length === 4, 'stats contain four threadId rows' );
				check( activeThreads.length >= 2, `at least two threads did both kinds of work (${ activeThreads.length })` );
				check( stats.candidateEvaluations > 8192 * 10, 'candidate evaluation total is substantial' );
				check( stats.mergedPairs === 8191, 'every face merge is counted exactly once' );
				check(
					threadRows.reduce( ( sum, row ) => sum + row.mergedPairs, 0 ) === 8191,
					'per-thread mergedPairs sum to hierarchy merge count',
				);
				check(
					worker._entries.every( entry => stats.threads[ entry.threadId ] ),
					'stats keys are real Node worker threadIds',
				);

			} finally {

				await worker.dispose();

			}

		},
	},
	{
		group: 'parallel',
		name: 'constructor validates worker count',
		run( check ) {

			checkThrows( check, () => new NodePLOCWorker( 0 ), /1 to 16/, 'zero workers' );
			checkThrows( check, () => new NodePLOCWorker( 17 ), /1 to 16/, 'too many workers' );
			checkThrows( check, () => new NodePLOCWorker( { workers: 1.5 } ), /1 to 16/, 'fractional workers' );

		},
	},
	{
		group: 'parallel',
		name: 'overlapping generate calls are explicitly rejected',
		async run( check ) {

			const worker = new NodePLOCWorker( { workers: 2 } );
			try {

				const first = worker.generate( makeGridGeometry( 4096 ), { verbose: false, targetLeafSize: 2 } );
				await checkRejects(
					check,
					() => worker.generate( makeGridGeometry( 64 ), { verbose: false } ),
					/already running/i,
					'overlapping generate',
				);
				const result = await first;
				check( result.bvh instanceof MeshBVH, 'first generate still completes' );

			} finally {

				await worker.dispose();

			}

		},
	},
	{
		group: 'parallel',
		name: 'worker failure rejects generation and dispose settles pending work and threads',
		async run( check ) {

			const failing = new NodePLOCWorker( { workers: 2 } );
			const generation = failing.generate( makeGridGeometry( 4096 ), { verbose: false } );
			failing._entries[ 0 ].worker.emit( 'error', new Error( 'synthetic worker failure' ) );
			await checkRejects( check, () => generation, /synthetic worker failure/, 'worker error' );
			await failing.dispose();
			check( failing._entries.length === 0, 'failed worker pool is closed' );
			await checkRejects( check, () => failing.generate( makeGridGeometry( 4 ) ), /disposed/i, 'generate after dispose' );

			const disposable = new NodePLOCWorker( { workers: 2 } );
			const pending = disposable.generate( makeGridGeometry( 8192 ), { verbose: false } );
			const disposePromise = disposable.dispose();
			let pendingError = null;
			pending.catch( error => {

				pendingError = error;

			} );
			await disposePromise;
			await new Promise( resolve => setTimeout( resolve, 30 ) );
			check( pendingError !== null && /dispose/i.test( pendingError.message ), 'dispose settles pending generate' );
			check( disposable._entries.length === 0, 'dispose terminates active threads' );

			let timedOut = false;
			await Promise.race( [
				pending.then( () => check( false, 'pending should not resolve after dispose' ), () => {} ),
				new Promise( resolve => setTimeout( () => {

					timedOut = true;
					resolve();

				}, 200 ) ),
			] );
			check( timedOut === false, 'pending Promise is not left hanging' );

		},
	},
	{
		group: 'mapping',
		name: 'direct PLOC matches Three per-face distance, faceIndex, UV and material through gaps',
		async run( check ) {

			await compareMappedRays( check, 'direct', false );

		},
	},
	{
		group: 'mapping',
		name: 'indirect PLOC matches Three per-face data without changing the input index',
		async run( check ) {

			await compareMappedRays( check, 'indirect', true );

		},
	},
	{
		group: 'mapping',
		name: 'repeated vertex movement and refit preserves exact ray mapping',
		async run( check ) {

			await compareMappedRays( check, 'refit-direct', false, { refitSteps: 3 } );
			await compareMappedRays( check, 'refit-indirect', true, { refitSteps: 2 } );
			check( true, 'multiple continuous refit steps completed' );

		},
	},
	{
		group: 'mapping',
		name: 'serialized PLOC roots restore query and non-indexed setIndex:false recovery',
		async run( check ) {

			await compareMappedRays( check, 'serialize-direct', false, { refitSteps: 1, serialize: true } );
			const indirectResult = await compareMappedRays( check, 'serialize-indirect', true, { refitSteps: 1, serialize: true } );
			check( indirectResult.geometry.index === null, 'indirect/non-indexed restore remains non-indexed with setIndex:false' );

		},
	},
	{
		group: 'mapping',
		name: 'drawRange coverage is preserved by PLOC roots and ray queries',
		async run( check ) {

			for ( const indirect of [ false, true ] ) {

				const positions = [];
				for ( let face = 0; face < 6; face ++ ) {

					const x = face * 1.2;
					positions.push( x, 0, 0, x + 1, 0, 0, x + .02, 1, 0 );

				}

				const geometry = new BufferGeometry();
				geometry.setAttribute( 'position', new Float32BufferAttribute( positions, 3 ) );
				geometry.setDrawRange( 3, 9 ); // vertex start/count: faces 1, 2, and 3.
				const bvh = new MeshBVH( geometry, { strategy: PLOC, indirect, targetLeafSize: 1, verbose: false } );
				const mesh = new Mesh( geometry, new MeshBasicMaterial( { side: DoubleSide } ) );
				const raycaster = new Raycaster();
				for ( let face = 0; face < 6; face ++ ) {

					raycaster.set( new Vector3( face * 1.2 + .28, .35, 2 ), new Vector3( 0, 0, - 1 ) );
					const expected = raycaster.intersectObject( mesh, false )[ 0 ] || null;
					const actual = bvh.raycastFirst( raycaster.ray, DoubleSide );
					if ( face >= 1 && face <= 3 ) {

						check( expected && actual, `drawRange ${ indirect ? 'indirect' : 'direct' } includes face ${ face }` );
						check( actual?.faceIndex === expected?.faceIndex, `drawRange faceIndex ${ face }` );

					} else {

						check( expected === null, `Three excludes drawRange face ${ face }` );
						check( actual === null, `PLOC excludes drawRange face ${ face }` );

					}

				}

			}

		},
	},
	{
		group: 'mapping',
		name: 'raycast, raycastFirst and shapecast all traverse the same packed PLOC tree',
		async run( check ) {

			const geometry = makeGridGeometry( 512 );
			const bvh = new MeshBVH( geometry, { strategy: PLOC, targetLeafSize: 6, verbose: false } );
			let raycaster = null;
			let first = null;
			let all = [];
			for ( let y = 1; y < 4 && ! first; y += .7 ) {

				for ( let x = 1; x < 130; x += .7 ) {

					raycaster = new Raycaster( new Vector3( x, y, 5 ), new Vector3( 0, 0, - 1 ) );
					first = bvh.raycastFirst( raycaster.ray, DoubleSide );
					if ( first ) {

						all = bvh.raycast( raycaster.ray, DoubleSide );
						break;

					}

				}

			}

			check( first !== null && all.length > 0, 'raycast APIs hit PLOC geometry' );
			check( all.some( hit => hit.faceIndex === first.faceIndex ), 'raycast and raycastFirst use same tree' );

			let leafCount = 0;
			bvh.traverse( ( depth, isLeaf ) => {

				if ( isLeaf ) leafCount ++;

			} );
			let rangeCalls = 0;
			bvh.shapecast( {
				intersectsBounds: () => true,
				intersectsRange: () => {

					rangeCalls ++;
					return false;

				},
			} );
			check( rangeCalls === leafCount && leafCount > 0, 'shapecast visits leaves from the packed PLOC tree' );

		},
	},
	{
		group: 'render',
		name: 'example/ploc.mjs renders a non-empty PPM through the PLOC tree',
		async run( check ) {

			const directory = fs.mkdtempSync( path.join( os.tmpdir(), 'ploc-preview-' ) );
			const ppmPath = path.join( directory, 'preview.ppm' );
			const examplePath = path.resolve( 'example/ploc.mjs' );
			const { stderr } = await execFileAsync( process.execPath, [ examplePath, ppmPath ], { timeout: 30000 } );
			check( stderr === '', `example should not write stderr: ${ stderr }` );
			check( fs.existsSync( ppmPath ) && fs.statSync( ppmPath ).size > 1024, 'PPM exists and contains image data' );

			const buffer = fs.readFileSync( ppmPath );
			const header = buffer.subarray( 0, 64 ).toString( 'latin1' );
			const match = header.match( /^P6\s+(\d+)\s+(\d+)\s+255\s/ );
			check( match !== null, 'PPM has binary P6 header' );
			if ( match ) {

				const width = Number( match[ 1 ] );
				const height = Number( match[ 2 ] );
				const headerLength = Buffer.byteLength( match[ 0 ], 'latin1' );
				const pixels = buffer.subarray( headerLength, headerLength + width * height * 3 );
				check( pixels.length === width * height * 3, 'PPM contains complete pixel data' );
				let nonBackground = 0;
				let colored = 0;
				for ( let i = 0; i < pixels.length; i += 3 ) {

					const r = pixels[ i ];
					const g = pixels[ i + 1 ];
					const b = pixels[ i + 2 ];
					if ( r + g + b > 45 ) nonBackground ++;
					if ( Math.max( r, g, b ) - Math.min( r, g, b ) > 18 ) colored ++;

				}

				check( nonBackground > width * height * .08, 'preview contains substantial rendered geometry' );
				check( colored > 100, 'preview contains shaded/colored pixels' );

			}

			fs.rmSync( directory, { recursive: true, force: true } );

		},
	},
];

export default cases;
