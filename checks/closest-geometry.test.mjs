import test from 'node:test';
import assert from 'node:assert/strict';
import {
	BufferGeometry,
	Float32BufferAttribute,
	Uint32BufferAttribute,
	Matrix4,
	Vector3,
	Vector2,
	Quaternion,
} from 'three';
import { MeshBVH } from '../src/core/MeshBVH.js';
import { ExtendedTriangle } from '../src/math/ExtendedTriangle.js';
import { setTriangle, getTriangleHitPointInfo } from '../src/utils/TriangleUtilities.js';

const EPS = 1e-5;
const POINT_EPS = 1e-4;

// deterministic PRNG so the shuffled input order is reproducible between runs
function mulberry32( seed ) {

	return function () {

		seed |= 0;
		seed = seed + 0x6D2B79F5 | 0;
		let t = Math.imul( seed ^ seed >>> 15, 1 | seed );
		t = t + Math.imul( t ^ t >>> 7, 61 | t ) ^ t;
		return ( ( t ^ t >>> 14 ) >>> 0 ) / 4294967296;

	};

}

function shuffleArray( array, seed ) {

	const rand = mulberry32( seed );
	for ( let i = array.length - 1; i > 0; i -- ) {

		const j = Math.floor( rand() * ( i + 1 ) );
		[ array[ i ], array[ j ] ] = [ array[ j ], array[ i ] ];

	}

	return array;

}

// Builds an indexed geometry from triangle definitions. The triangles are intentionally stored in
// a shuffled order so an indirect BVH has to reorder them spatially. Every triangle gets unique
// vertices which means UVs can differ even between triangles sharing the same positions.
function buildGeometry( triDefs, seed = 1 ) {

	const defs = triDefs.map( ( d, id ) => ( { ...d, id } ) );
	shuffleArray( defs, seed );

	const positions = [];
	const uvs = [];
	const indices = [];
	defs.forEach( d => {

		const base = positions.length / 3;
		for ( const v of d.verts ) positions.push( v[ 0 ], v[ 1 ], v[ 2 ] );
		for ( const uv of d.uvs ) uvs.push( uv[ 0 ], uv[ 1 ] );
		indices.push( base, base + 1, base + 2 );

	} );

	const geometry = new BufferGeometry();
	geometry.setAttribute( 'position', new Float32BufferAttribute( positions, 3 ) );
	geometry.setAttribute( 'uv', new Float32BufferAttribute( uvs, 2 ) );
	geometry.setIndex( new Uint32BufferAttribute( indices, 1 ) );

	// one group per contiguous run of the same material
	let runStart = 0;
	for ( let i = 1; i <= defs.length; i ++ ) {

		if ( i === defs.length || defs[ i ].material !== defs[ i - 1 ].material ) {

			geometry.addGroup( runStart * 3, ( i - runStart ) * 3, defs[ runStart ].material );
			runStart = i;

		}

	}

	geometry.computeBoundingBox();
	return { geometry, order: defs.map( d => d.id ) };

}

function triCount( geometry ) {

	return geometry.index.count / 3;

}

function makeTriangle( geometry, triangleIndex, target ) {

	setTriangle( target, triangleIndex * 3, geometry.index, geometry.attributes.position );
	target.needsUpdate = true;
	return target;

}

// Independent brute-force reference. It reuses the triangle distance primitive but never touches
// a BVH traversal, so the expected value is independent of the code under test.
function referenceClosest( geometryA, geometryB, matrix, maxThreshold = Infinity ) {

	const triA = new ExtendedTriangle();
	const triB = new ExtendedTriangle();
	const point1 = new Vector3();
	const point2 = new Vector3();
	let best = null;

	for ( let i = 0, l = triCount( geometryA ); i < l; i ++ ) {

		makeTriangle( geometryA, i, triA );
		for ( let j = 0, l2 = triCount( geometryB ); j < l2; j ++ ) {

			makeTriangle( geometryB, j, triB );
			triB.a.applyMatrix4( matrix );
			triB.b.applyMatrix4( matrix );
			triB.c.applyMatrix4( matrix );
			triB.needsUpdate = true;

			const distance = triA.distanceToTriangle( triB, point1, point2 );
			if ( distance < maxThreshold && ( best === null || distance < best.distance ) ) {

				best = {
					distance,
					triangleA: i,
					triangleB: j,
					point1: point1.clone(),
					point2: point2.clone(),
				};

			}

		}

	}

	return best;

}

function pairDistance( geometryA, geometryB, triangleIndexA, triangleIndexB, matrix ) {

	const triA = makeTriangle( geometryA, triangleIndexA, new ExtendedTriangle() );
	const triB = makeTriangle( geometryB, triangleIndexB, new ExtendedTriangle() );
	triB.a.applyMatrix4( matrix );
	triB.b.applyMatrix4( matrix );
	triB.c.applyMatrix4( matrix );
	triB.needsUpdate = true;
	return triA.distanceToTriangle( triB );

}

function expectedMaterial( geometry, triangleIndex ) {

	const firstIndex = triangleIndex * 3;
	for ( const group of geometry.groups ) {

		if ( firstIndex >= group.start && firstIndex < group.start + group.count ) {

			return group.materialIndex;

		}

	}

	return 0;

}

// Checks the point really lies on the reported triangle and that getTriangleHitPointInfo returns
// the UVs and material associated with that exact triangle.
function assertPointOnReportedFace( geometry, triangleIndex, point, label ) {

	const info = getTriangleHitPointInfo( point, geometry, triangleIndex, {} );
	const { barycoord, face, uv } = info;

	assert.ok( barycoord.x >= - POINT_EPS, `${label}: barycoord x` );
	assert.ok( barycoord.y >= - POINT_EPS, `${label}: barycoord y` );
	assert.ok( barycoord.z >= - POINT_EPS, `${label}: barycoord z` );
	assert.ok( Math.abs( barycoord.x + barycoord.y + barycoord.z - 1 ) < POINT_EPS, `${label}: barycoord sum` );

	// reconstruct the point from the face vertices using the reported barycentric coordinates
	const position = geometry.attributes.position;
	const reconstructed = new Vector3()
		.addScaledVector( new Vector3().fromBufferAttribute( position, face.a ), barycoord.x )
		.addScaledVector( new Vector3().fromBufferAttribute( position, face.b ), barycoord.y )
		.addScaledVector( new Vector3().fromBufferAttribute( position, face.c ), barycoord.z );
	assert.ok( reconstructed.distanceTo( point ) < POINT_EPS, `${label}: point lies on reported face` );

	// independently compute the expected UV interpolation from the triangle's own corner UVs
	const uvAttribute = geometry.attributes.uv;
	const uvA = new Vector2().fromBufferAttribute( uvAttribute, face.a );
	const uvB = new Vector2().fromBufferAttribute( uvAttribute, face.b );
	const uvC = new Vector2().fromBufferAttribute( uvAttribute, face.c );
	const expectedUv = new Vector2()
		.addScaledVector( uvA, barycoord.x )
		.addScaledVector( uvB, barycoord.y )
		.addScaledVector( uvC, barycoord.z );
	assert.ok( uv !== null, `${label}: uv present` );
	assert.ok( Math.abs( uv.x - expectedUv.x ) < POINT_EPS, `${label}: uv x` );
	assert.ok( Math.abs( uv.y - expectedUv.y ) < POINT_EPS, `${label}: uv y` );

	assert.equal( face.materialIndex, expectedMaterial( geometry, triangleIndex ), `${label}: material index` );
	return info;

}

function assertResultMatchesReference( result, target2, geometryA, geometryB, matrix, reference, label ) {

	assert.ok( result, `${label}: result is not null` );
	const tolerance = EPS * Math.max( 1, reference.distance );
	assert.ok( Math.abs( result.distance - reference.distance ) <= tolerance, `${label}: distance ${result.distance} vs ${reference.distance}` );

	// the reported faces have to be a real closest pair, not just faces near the closest points
	const reportedDistance = pairDistance( geometryA, geometryB, result.faceIndex, target2.faceIndex, matrix );
	assert.ok( Math.abs( reportedDistance - reference.distance ) <= tolerance, `${label}: reported pair is closest` );

	assertPointOnReportedFace( geometryA, result.faceIndex, result.point, `${label} target1` );
	assertPointOnReportedFace( geometryB, target2.faceIndex, target2.point, `${label} target2` );

	// point1 lives in the BVH frame, point2 in the other geometry's local frame - transforming
	// point2 into the BVH frame has to reproduce the reported distance
	const point2InBvh = target2.point.clone().applyMatrix4( matrix );
	assert.ok( Math.abs( point2InBvh.distanceTo( result.point ) - result.distance ) <= tolerance, `${label}: cross frame points` );

	// target2.distance is measured in the other geometry's local frame, which is a different
	// quantity under non-uniform scaling
	const inverse = matrix.clone().invert();
	const point1InOther = result.point.clone().applyMatrix4( inverse );
	assert.ok( Math.abs( point1InOther.distanceTo( target2.point ) - target2.distance ) < POINT_EPS, `${label}: target2 local distance` );

}

function attachTree( geometry, mode ) {

	if ( mode !== 'none' ) {

		geometry.boundsTree = new MeshBVH( geometry, {
			targetLeafSize: 1,
			indirect: mode === 'indirect',
		} );

	}

}

// every combination of direct / indirect first tree and no / direct / indirect second tree
const MODE_MATRIX = [
	[ 'direct', 'none' ],
	[ 'direct', 'direct' ],
	[ 'direct', 'indirect' ],
	[ 'indirect', 'none' ],
	[ 'indirect', 'direct' ],
	[ 'indirect', 'indirect' ],
];

function yzTriangle( x, id, material ) {

	return {
		verts: [ [ x, 0, 0 ], [ x, 1, 0 ], [ x, 0, 1 ] ],
		uvs: [ [ id, id * 2 ], [ id + 0.1, id * 2 + 0.2 ], [ id + 0.2, id * 2 + 0.4 ] ],
		material,
	};

}

test( 'closest-geometry reproduces the baseline query with and without a second bounds tree', async t => {

	const xs = [ 90, 70, 50, 30, 80, 60, 40, 2 ];
	const matrix = new Matrix4().makeScale( 0.01, 1, 1 );

	for ( const [ modeA, modeB ] of MODE_MATRIX ) {

		await t.test( `${modeA} first tree, ${modeB} second tree`, () => {

			const builtA = buildGeometry( [ yzTriangle( 0, 0, 0 ) ], 11 );
			const builtB = buildGeometry( xs.map( ( x, i ) => yzTriangle( x, i + 1, i % 2 ) ), 23 );
			const tree = new MeshBVH( builtA.geometry, { targetLeafSize: 1, indirect: modeA === 'indirect' } );
			attachTree( builtB.geometry, modeB );

			const target1 = {};
			const target2 = {};
			const result = tree.closestPointToGeometry( builtB.geometry, matrix, target1, target2, 0, 0.1 );
			assert.ok( result, 'a result within maxThreshold is returned' );
			assert.ok( Math.abs( result.distance - 0.02 ) < EPS, 'distance 0.02 regardless of tree layout' );
			assert.equal( result, target1 );

		} );

	}

} );

test( 'closest-geometry shuffled separated geometries match the brute force reference across all modes', async t => {

	const defsA = [
		yzTriangle( 0, 1, 1 ),
		yzTriangle( 0.4, 2, 0 ),
		{ verts: [ [ - 1, - 1, 0 ], [ - 1, 0, 0 ], [ - 1, - 1, 1 ] ], uvs: [ [ 3, 6 ], [ 3.3, 6.2 ], [ 3.1, 6.7 ] ], material: 1 },
		{ verts: [ [ 0.2, 2, 0.1 ], [ 0.2, 3, 0.1 ], [ 0.9, 2, 0.8 ] ], uvs: [ [ 4, 8 ], [ 4.4, 8.1 ], [ 4.8, 8.9 ] ], material: 0 },
		{ verts: [ [ 0.6, - 2, 0.2 ], [ 0.6, - 1, 0.2 ], [ 1.3, - 2, 0.9 ] ], uvs: [ [ 5, 10 ], [ 5.5, 10.5 ], [ 5.1, 10.1 ] ], material: 1 },
	];
	const defsB = [
		yzTriangle( 0, 11, 0 ),
		yzTriangle( 0.5, 12, 1 ),
		{ verts: [ [ 1, 1, 0 ], [ 1, 2, 0 ], [ 1, 1, 1 ] ], uvs: [ [ 13, 26 ], [ 13.3, 26.3 ], [ 13.8, 26.1 ] ], material: 0 },
		{ verts: [ [ - 2, 0.4, - 0.5 ], [ - 2, 1.4, - 0.5 ], [ - 2, 0.4, 0.5 ] ], uvs: [ [ 14, 28 ], [ 14.2, 28.7 ], [ 14.9, 28.2 ] ], material: 1 },
		{ verts: [ [ 0.3, - 1.5, 1.5 ], [ 0.3, - 0.5, 1.5 ], [ 1.1, - 1.5, 2.2 ] ], uvs: [ [ 15, 30 ], [ 15.6, 30.6 ], [ 15.2, 30.1 ] ], material: 0 },
	];
	const quaternion = new Quaternion().setFromAxisAngle( new Vector3( 0.3, 0.8, 0.5 ).normalize(), 0.7 );
	const matrix = new Matrix4().compose( new Vector3( 3, 1, - 2 ), quaternion, new Vector3( 0.5, 1.3, 0.8 ) );

	for ( const [ modeA, modeB ] of MODE_MATRIX ) {

		await t.test( `${modeA} first tree, ${modeB} second tree`, () => {

			const builtA = buildGeometry( defsA, 101 );
			const builtB = buildGeometry( defsB, 202 );
			const originalIndexA = Array.from( builtA.geometry.index.array );
			const originalIndexB = Array.from( builtB.geometry.index.array );

			const tree = new MeshBVH( builtA.geometry, { targetLeafSize: 1, indirect: modeA === 'indirect' } );
			attachTree( builtB.geometry, modeB );

			// indirect mode must not reorder the geometry's own index buffer while direct mode is
			// allowed to
			if ( modeA === 'indirect' ) {

				assert.deepEqual( Array.from( builtA.geometry.index.array ), originalIndexA, 'first geometry index stays untouched' );

			}

			if ( modeB === 'indirect' ) {

				assert.deepEqual( Array.from( builtB.geometry.index.array ), originalIndexB, 'second geometry index stays untouched' );

			}

			const target1 = { point: new Vector3( 7, 7, 7 ) };
			const target2 = { point: new Vector3( 9, 9, 9 ) };
			const result = tree.closestPointToGeometry( builtB.geometry, matrix, target1, target2 );
			const reference = referenceClosest( builtA.geometry, builtB.geometry, matrix );

			assertResultMatchesReference( result, target2, builtA.geometry, builtB.geometry, matrix, reference, 'separated' );

			// the preallocated point objects have to be reused rather than replaced
			assert.equal( result.point, target1.point );
			assert.equal( target2.point, target2.point );

		} );

	}

} );

test( 'closest-geometry triangles sharing positions but with different UVs resolve the reported face correctly', async t => {

	// two triangles occupy the exact same positions but carry distinct UVs and material groups
	const sharedVerts = [ [ 5, 0, 0 ], [ 5, 1, 0 ], [ 5, 0, 1 ] ];
	const defsB = [
		{ verts: sharedVerts.map( v => [ ...v ] ), uvs: [ [ 0, 0 ], [ 10, 0 ], [ 0, 10 ] ], material: 0 },
		{ verts: sharedVerts.map( v => [ ...v ] ), uvs: [ [ 5, 5 ], [ 7, 5 ], [ 5, 7 ] ], material: 1 },
		{ verts: [ [ 8, - 2, - 2 ], [ 8, - 1, - 2 ], [ 8, - 2, - 1 ] ], uvs: [ [ 20, 20 ], [ 21, 20 ], [ 20, 21 ] ], material: 0 },
	];
	const defsA = [ yzTriangle( 0, 31, 1 ) ];
	const matrix = new Matrix4();

	for ( const [ modeA, modeB ] of MODE_MATRIX ) {

		await t.test( `${modeA} first tree, ${modeB} second tree`, () => {

			const builtA = buildGeometry( defsA, 301 );
			const builtB = buildGeometry( defsB, 401 );
			const tree = new MeshBVH( builtA.geometry, { targetLeafSize: 1, indirect: modeA === 'indirect' } );
			attachTree( builtB.geometry, modeB );

			const target1 = {};
			const target2 = {};
			const result = tree.closestPointToGeometry( builtB.geometry, matrix, target1, target2 );
			const reference = referenceClosest( builtA.geometry, builtB.geometry, matrix );

			assertResultMatchesReference( result, target2, builtA.geometry, builtB.geometry, matrix, reference, 'shared positions' );

			// whichever coincident triangle won the tie, hit info has to match that triangle's own UVs
			const info = assertPointOnReportedFace( builtB.geometry, target2.faceIndex, target2.point, 'shared positions target2' );
			assert.ok( info.uv.x >= 0 && info.uv.y >= 0, 'uvs come from the reported triangle' );
			assert.equal( info.face.materialIndex, expectedMaterial( builtB.geometry, target2.faceIndex ) );

		} );

	}

} );

test( 'closest-geometry intersecting geometries return zero distance with valid faces', async t => {

	const defsA = [
		{ verts: [ [ 0, - 1, - 1 ], [ 0, 1, - 1 ], [ 0, - 1, 1 ] ], uvs: [ [ 0, 0 ], [ 1, 0 ], [ 0, 1 ] ], material: 0 },
		yzTriangle( 4, 41, 1 ),
	];
	const defsB = [
		// triangle in the y = 0 plane which cuts through the x = 0 plane triangle above
		{ verts: [ [ - 0.5, 0, - 0.5 ], [ 0.5, 0, - 0.5 ], [ - 0.5, 0, 0.5 ] ], uvs: [ [ 2, 2 ], [ 3, 2 ], [ 2, 3 ] ], material: 1 },
		yzTriangle( 6, 42, 0 ),
	];
	const matrix = new Matrix4().makeTranslation( 0, 0.2, 0.2 );

	for ( const [ modeA, modeB ] of MODE_MATRIX ) {

		await t.test( `${modeA} first tree, ${modeB} second tree`, () => {

			const builtA = buildGeometry( defsA, 501 );
			const builtB = buildGeometry( defsB, 601 );
			const tree = new MeshBVH( builtA.geometry, { targetLeafSize: 1, indirect: modeA === 'indirect' } );
			attachTree( builtB.geometry, modeB );

			const target1 = {};
			const target2 = {};
			const result = tree.closestPointToGeometry( builtB.geometry, matrix, target1, target2, 0, 0.5 );
			const reference = referenceClosest( builtA.geometry, builtB.geometry, matrix, 0.5 );

			assertResultMatchesReference( result, target2, builtA.geometry, builtB.geometry, matrix, reference, 'intersecting' );
			assert.ok( reference.distance < EPS, 'the reference pair intersects' );
			assert.ok( result.distance < EPS, 'the reported distance is zero' );

		} );

	}

} );

test( 'closest-geometry negative scale and composed transforms match the reference', async t => {

	const defsA = [
		yzTriangle( 0, 51, 0 ),
		{ verts: [ [ 0.5, 1.5, - 0.5 ], [ 0.5, 2.5, - 0.5 ], [ 1.2, 1.5, 0.4 ] ], uvs: [ [ 51, 1 ], [ 51.5, 1.7 ], [ 51.2, 1.2 ] ], material: 1 },
		{ verts: [ [ - 0.5, - 1.5, 0.4 ], [ - 0.5, - 0.5, 0.4 ], [ 0.2, - 1.5, 1.1 ] ], uvs: [ [ 52, 2 ], [ 52.5, 2.7 ], [ 52.2, 2.2 ] ], material: 0 },
		yzTriangle( 1, 53, 1 ),
	];
	const defsB = [
		yzTriangle( 0, 61, 1 ),
		{ verts: [ [ 0.4, 0.5, 0.6 ], [ 0.4, 1.3, 0.6 ], [ 1.1, 0.5, 1.3 ] ], uvs: [ [ 61, 3 ], [ 61.4, 3.4 ], [ 61.9, 3.1 ] ], material: 0 },
		{ verts: [ [ - 0.6, - 0.8, - 0.3 ], [ - 0.6, 0.1, - 0.3 ], [ 0.1, - 0.8, 0.5 ] ], uvs: [ [ 62, 4 ], [ 62.6, 4.6 ], [ 62.1, 4.2 ] ], material: 1 },
		yzTriangle( - 1, 64, 0 ),
	];
	const quaternion = new Quaternion().setFromAxisAngle( new Vector3( 0.2, 1, 0.4 ).normalize(), 0.9 );
	// negative x scale gives the matrix a negative determinant
	const matrix = new Matrix4().compose( new Vector3( 2, 0.3, - 1 ), quaternion, new Vector3( - 1.2, 0.6, 1 ) );
	assert.ok( matrix.determinant() < 0, 'the transform really contains a negative scale' );

	for ( const [ modeA, modeB ] of MODE_MATRIX ) {

		await t.test( `${modeA} first tree, ${modeB} second tree`, () => {

			const builtA = buildGeometry( defsA, 701 );
			const builtB = buildGeometry( defsB, 801 );
			const tree = new MeshBVH( builtA.geometry, { targetLeafSize: 1, indirect: modeA === 'indirect' } );
			attachTree( builtB.geometry, modeB );

			const target1 = {};
			const target2 = {};
			const result = tree.closestPointToGeometry( builtB.geometry, matrix, target1, target2 );
			const reference = referenceClosest( builtA.geometry, builtB.geometry, matrix );

			assertResultMatchesReference( result, target2, builtA.geometry, builtB.geometry, matrix, reference, 'negative scale' );

		} );

	}

} );

test( 'closest-geometry threshold edges return null without modifying the target objects', async t => {

	const defsA = [ yzTriangle( 0, 71, 1 ), yzTriangle( 0.3, 72, 0 ) ];
	const defsB = [ yzTriangle( 2, 81, 0 ), yzTriangle( 3, 82, 1 ) ];
	const matrix = new Matrix4();

	for ( const [ modeA, modeB ] of MODE_MATRIX ) {

		await t.test( `${modeA} first tree, ${modeB} second tree`, () => {

			const builtA = buildGeometry( defsA, 901 );
			const builtB = buildGeometry( defsB, 902 );
			const tree = new MeshBVH( builtA.geometry, { targetLeafSize: 1, indirect: modeA === 'indirect' } );
			attachTree( builtB.geometry, modeB );

			const reference = referenceClosest( builtA.geometry, builtB.geometry, matrix );
			assert.ok( reference.distance > 0, 'the geometries are fully separated' );

			// below maxThreshold: null and untouched targets including their point objects
			const target1 = { point: new Vector3( 123, 456, 789 ), distance: 7, faceIndex: 99 };
			const target2 = { point: new Vector3( 321, 654, 987 ), distance: 8, faceIndex: 98 };
			const missing = tree.closestPointToGeometry(
				builtB.geometry, matrix, target1, target2, 0, reference.distance * 0.5,
			);
			assert.equal( missing, null );
			assert.deepEqual( target1.point.toArray(), [ 123, 456, 789 ] );
			assert.deepEqual( target2.point.toArray(), [ 321, 654, 987 ] );
			assert.equal( target1.distance, 7 );
			assert.equal( target1.faceIndex, 99 );
			assert.equal( target2.distance, 8 );
			assert.equal( target2.faceIndex, 98 );

			// above maxThreshold: the closest pair is found
			const found = tree.closestPointToGeometry(
				builtB.geometry, matrix, target1, target2, 0, reference.distance * 1.5,
			);
			assert.ok( found );
			assert.ok( Math.abs( found.distance - reference.distance ) < EPS );
			assertResultMatchesReference( found, target2, builtA.geometry, builtB.geometry, matrix, reference, 'threshold above' );

			// minThreshold above the closest distance triggers the early return with a valid pair
			const early = tree.closestPointToGeometry(
				builtB.geometry, matrix, {}, {}, reference.distance * 1.2, Infinity,
			);
			assert.ok( early );
			assert.ok( early.distance < reference.distance * 1.2 );
			assertPointOnReportedFace( builtA.geometry, early.faceIndex, early.point, 'min threshold target1' );

			// a null target2 has to keep working
			const single = tree.closestPointToGeometry( builtB.geometry, matrix, {}, null );
			assert.ok( single );
			assert.ok( Math.abs( single.distance - reference.distance ) < EPS );

		} );

	}

} );

test( 'closest-geometry consecutive queries and refit do not reuse stale results', async t => {

	const defsA = [ yzTriangle( 0, 91, 1 ), yzTriangle( 0.4, 92, 0 ) ];
	const defsB = [ yzTriangle( 2, 101, 0 ), yzTriangle( 5, 102, 1 ) ];
	const matrix1 = new Matrix4();
	const matrix2 = new Matrix4().makeTranslation( 10, 0, 0 );

	for ( const modeB of [ 'direct', 'indirect' ] ) {

		await t.test( `second tree ${modeB}`, () => {

			const builtA = buildGeometry( defsA, 1001 );
			const builtB = buildGeometry( defsB, 1002 );
			const tree = new MeshBVH( builtA.geometry, { targetLeafSize: 1 } );
			attachTree( builtB.geometry, modeB );

			const target1 = {};
			const target2 = {};
			const first = tree.closestPointToGeometry( builtB.geometry, matrix1, target1, target2 );
			const reference1 = referenceClosest( builtA.geometry, builtB.geometry, matrix1 );
			assertResultMatchesReference( first, target2, builtA.geometry, builtB.geometry, matrix1, reference1, 'first query' );

			// a second, fully displaced query on the same BVH objects must not leak the first result
			const target1b = {};
			const target2b = {};
			const second = tree.closestPointToGeometry( builtB.geometry, matrix2, target1b, target2b );
			const reference2 = referenceClosest( builtA.geometry, builtB.geometry, matrix2 );
			assertResultMatchesReference( second, target2b, builtA.geometry, builtB.geometry, matrix2, reference2, 'second query' );
			assert.ok( Math.abs( second.distance - first.distance ) > EPS, 'the queries have different answers' );

			// move the previously closest triangle far away, refit, and query again
			const position = builtB.geometry.attributes.position;
			const closestTri = reference1.triangleB;
			for ( let k = 0; k < 3; k ++ ) position.setX( closestTri * 3 + k, position.getX( closestTri * 3 + k ) + 20 );
			position.needsUpdate = true;
			builtB.geometry.boundsTree.refit();
			builtB.geometry.computeBoundingBox();

			const target1c = {};
			const target2c = {};
			const refitted = tree.closestPointToGeometry( builtB.geometry, matrix1, target1c, target2c );
			const reference3 = referenceClosest( builtA.geometry, builtB.geometry, matrix1 );
			assertResultMatchesReference( refitted, target2c, builtA.geometry, builtB.geometry, matrix1, reference3, 'after refit' );
			assert.ok( Math.abs( refitted.distance - first.distance ) > EPS, 'refit changed the closest pair' );

		} );

	}

} );

test( 'closest-geometry the second bounds tree still prunes narrow phase triangle tests', () => {

	// one small facing triangle on the first geometry
	const defsA = [
		yzTriangle( 0, 201, 0 ),
		{ verts: [ [ 0, 1.5, 1.5 ], [ 0, 2.2, 1.5 ], [ 0.7, 1.5, 2.2 ] ], uvs: [ [ 201, 1 ], [ 201.7, 1.1 ], [ 201.2, 1.7 ] ], material: 1 },
	];

	// one genuinely close triangle hidden among a large number of far away, spatially shuffled ones
	const defsB = [ yzTriangle( 2, 301, 1 ) ];
	for ( let i = 0; i < 238; i ++ ) {

		const x = 30 + ( i % 40 ) * 3;
		const y = - 20 + Math.floor( i / 40 ) * 8;
		defsB.push( {
			verts: [ [ x, y, 0 ], [ x, y + 1, 0 ], [ x, y, 1 ] ],
			uvs: [ [ 302 + i, i ], [ 302 + i + 0.1, i + 0.3 ], [ 302 + i + 0.2, i + 0.7 ] ],
			material: i % 2,
		} );

	}

	const matrix = new Matrix4();
	const fullPairCount = triCount( buildGeometry( defsA, 1 ).geometry ) * triCount( buildGeometry( defsB, 2 ).geometry );

	const distanceProto = ExtendedTriangle.prototype;
	const originalDistanceToTriangle = distanceProto.distanceToTriangle;
	let calls = 0;
	distanceProto.distanceToTriangle = function ( ...args ) {

		calls ++;
		return originalDistanceToTriangle.apply( this, args );

	};

	try {

		const counts = {};
		for ( const modeB of [ 'none', 'direct', 'indirect' ] ) {

			const builtA = buildGeometry( defsA, 1101 );
			const builtB = buildGeometry( defsB, 1202 );
			const tree = new MeshBVH( builtA.geometry, { targetLeafSize: 1 } );
			attachTree( builtB.geometry, modeB );

			calls = 0;
			const result = tree.closestPointToGeometry( builtB.geometry, matrix, {}, {} );
			counts[ modeB ] = calls;

			const reference = referenceClosest( builtA.geometry, builtB.geometry, matrix );
			assert.ok( Math.abs( result.distance - reference.distance ) < EPS, `${modeB}: pruned result still matches reference` );

		}

		console.log( 'narrow phase triangle distance calls:', counts );
		assert.equal( counts.none, fullPairCount, 'without a tree every triangle pair is tested' );
		assert.ok( counts.direct < counts.none / 2, 'direct tree prunes most irrelevant triangles' );
		assert.ok( counts.indirect < counts.none / 2, 'indirect tree prunes most irrelevant triangles' );

	} finally {

		distanceProto.distanceToTriangle = originalDistanceToTriangle;

	}

} );
