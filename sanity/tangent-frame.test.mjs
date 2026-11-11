import { test } from 'node:test';
import assert from 'node:assert/strict';
import {
	BoxGeometry,
	Float32BufferAttribute,
	Matrix3,
	Matrix4,
	MeshBasicMaterial,
	Plane,
	Triangle,
	Vector3,
} from 'three';
import {
	ADDITION,
	INTERSECTION,
	SUBTRACTION,
	Brush,
	Evaluator,
} from '../src/index.js';

// Tangent frame convention under test (see GeometryBuilder.js):
//
// - "tangent" is a float4 attribute: xyz is the tangent along U, w is exactly
//   ±1 and the bitangent is reconstructed as B = w * cross( N, T ). Every
//   source triangle has a uniform w on all three vertices.
// - A result vertex (untouched or produced by a cut) interpolates the source
//   normal and tangent xyz with the barycentric coordinates within its source
//   triangle, transforms the normal by the inverse-transpose and the tangent
//   xyz by the linear part of the relative transform, then orthogonalizes and
//   normalizes the tangent against the output normal.
// - w follows the source triangle and flips once for a reflection in the
//   relative transform and once more when the boolean operation inverts the
//   source surface (eg subtraction inner walls). The tangent direction along U
//   never flips with surface inversion.

const EPS = 1e-5;

// fixed tangent-space normal used to validate the reconstructed frame direction
const TS_NORMAL = new Vector3( 0.3, - 0.4, Math.sqrt( 0.75 ) );

// box faces (four consecutive vertices each) whose u coordinate is mirrored so
// that computeTangents produces w = -1 for them
const MIRRORED_FACES_A = [ 1, 3, 5 ];
const MIRRORED_FACES_B = [ 0, 2, 4 ];

const _verts = [ new Vector3(), new Vector3(), new Vector3() ];
const _tri = new Triangle();
const _plane = new Plane();
const _bary = new Vector3();
const _v0 = new Vector3();
const _v1 = new Vector3();
const _v2 = new Vector3();
const _normalMatrix = new Matrix3();
const _linearMatrix = new Matrix3();

function makeGeometry( mirroredFaces ) {

	const geometry = new BoxGeometry();

	// mirror the u coordinate of the requested faces. Each box face owns four
	// consecutive vertices so the mirrored charts stay split from the rest.
	const uv = geometry.attributes.uv;
	for ( const face of mirroredFaces ) {

		for ( let v = 4 * face; v < 4 * face + 4; v ++ ) {

			uv.setX( v, 1 - uv.getX( v ) );

		}

	}

	geometry.computeTangents();

	// a plain non-directional attribute to check that interpolation of
	// non-directional data is left untouched
	const count = geometry.attributes.position.count;
	const colors = new Float32Array( count * 3 );
	for ( let i = 0; i < count; i ++ ) {

		colors[ 3 * i + 0 ] = ( i % 8 ) / 7;
		colors[ 3 * i + 1 ] = ( i % 5 ) / 4;
		colors[ 3 * i + 2 ] = ( i % 3 ) / 2;

	}

	geometry.setAttribute( 'color', new Float32BufferAttribute( colors, 3 ) );
	return geometry;

}

function makeBrush( mirroredFaces, {
	position = [ 0, 0, 0 ],
	rotation = [ 0, 0, 0 ],
	scale = [ 1, 1, 1 ],
} = {} ) {

	const brush = new Brush( makeGeometry( mirroredFaces ) );
	brush.position.set( ...position );
	brush.rotation.set( ...rotation );
	brush.scale.set( ...scale );
	brush.updateMatrixWorld();
	return brush;

}

// two obliquely intersecting boxes with rotation and non-uniform scale on both
// sides and mirrored (negative w) faces on both sides
function makeIntersectingPair() {

	const a = makeBrush( MIRRORED_FACES_A, {
		position: [ 0.1, - 0.2, 0.05 ],
		rotation: [ 0.2, 0.4, - 0.3 ],
		scale: [ 1.2, 0.9, 1.1 ],
	} );
	const b = makeBrush( MIRRORED_FACES_B, {
		position: [ 0.45, 0.35, 0.25 ],
		rotation: [ 0.5, 0.7, 0.3 ],
		scale: [ 0.8, 1.1, 0.9 ],
	} );
	return { a, b };

}

function readTriVerts( geometry, triIndex, matrix ) {

	const index = geometry.index;
	const pos = geometry.attributes.position;
	for ( let k = 0; k < 3; k ++ ) {

		const i = index ? index.getX( 3 * triIndex + k ) : 3 * triIndex + k;
		_verts[ k ].fromBufferAttribute( pos, i );
		if ( matrix ) {

			_verts[ k ].applyMatrix4( matrix );

		}

	}

	return _verts;

}

// finds the source triangle whose plane carries the given point (in the result
// frame, ie brush A's local frame). Returns the source slot and triangle index.
function findSourceTriangle( point, sources ) {

	let best = null;
	for ( let s = 0; s < sources.length; s ++ ) {

		const { geometry, matrix } = sources[ s ];
		const index = geometry.index;
		const triCount = index ? index.count / 3 : geometry.attributes.position.count / 3;
		for ( let t = 0; t < triCount; t ++ ) {

			const verts = readTriVerts( geometry, t, matrix );
			_tri.a.copy( verts[ 0 ] );
			_tri.b.copy( verts[ 1 ] );
			_tri.c.copy( verts[ 2 ] );
			_tri.getPlane( _plane );
			const dist = Math.abs( _plane.distanceToPoint( point ) );
			if ( dist > 1e-4 ) {

				continue;

			}

			_tri.getBarycoord( point, _bary );
			if ( _bary.x < - 1e-4 || _bary.y < - 1e-4 || _bary.z < - 1e-4 ) {

				continue;

			}

			if ( ! best || dist < best.dist ) {

				best = { source: s, triIndex: t, dist };

			}

		}

	}

	return best;

}

// independently computes the expected output frame for a point on a source
// triangle following the documented convention
function expectedFrame( geometry, triIndex, bary, relMatrix, surfaceInverted ) {

	const index = geometry.index;
	const normalAttr = geometry.attributes.normal;
	const tangentAttr = geometry.attributes.tangent;
	const i0 = index.getX( 3 * triIndex );
	const i1 = index.getX( 3 * triIndex + 1 );
	const i2 = index.getX( 3 * triIndex + 2 );

	// interpolate the source-space normal and tangent direction
	const n = new Vector3()
		.addScaledVector( _v0.fromBufferAttribute( normalAttr, i0 ), bary.x )
		.addScaledVector( _v1.fromBufferAttribute( normalAttr, i1 ), bary.y )
		.addScaledVector( _v2.fromBufferAttribute( normalAttr, i2 ), bary.z );
	const t = new Vector3()
		.addScaledVector( _v0.fromBufferAttribute( tangentAttr, i0 ), bary.x )
		.addScaledVector( _v1.fromBufferAttribute( tangentAttr, i1 ), bary.y )
		.addScaledVector( _v2.fromBufferAttribute( tangentAttr, i2 ), bary.z );
	const wSrc = tangentAttr.getW( i0 ) < 0 ? - 1 : 1;

	// normal by the inverse-transpose, tangent by the linear part
	_normalMatrix.getNormalMatrix( relMatrix );
	_linearMatrix.setFromMatrix4( relMatrix );

	const N = n.applyMatrix3( _normalMatrix ).normalize();
	if ( surfaceInverted ) {

		N.negate();

	}

	const T = t.applyMatrix3( _linearMatrix );
	T.addScaledVector( N, - T.dot( N ) ).normalize();

	// w flips once for a reflection in the relative transform and once for a
	// boolean surface inversion
	let w = wSrc;
	if ( relMatrix.determinant() < 0 ) {

		w = - w;

	}

	if ( surfaceInverted ) {

		w = - w;

	}

	return { N, T, w };

}

function reconstructTangentSpaceNormal( T, N, w ) {

	const B = new Vector3().crossVectors( N, T ).multiplyScalar( w );
	return new Vector3()
		.addScaledVector( T, TS_NORMAL.x )
		.addScaledVector( B, TS_NORMAL.y )
		.addScaledVector( N, TS_NORMAL.z )
		.normalize();

}

function assertVectorNear( actual, expected, eps, label ) {

	assert.ok(
		Math.abs( actual.x - expected.x ) < eps &&
		Math.abs( actual.y - expected.y ) < eps &&
		Math.abs( actual.z - expected.z ) < eps,
		`${ label }: expected (${ expected.x.toFixed( 5 ) }, ${ expected.y.toFixed( 5 ) }, ${ expected.z.toFixed( 5 ) }), ` +
		`got (${ actual.x.toFixed( 5 ) }, ${ actual.y.toFixed( 5 ) }, ${ actual.z.toFixed( 5 ) })`,
	);

}

function verifyVertexFrame( geometry, vertexIndex, expected, label ) {

	const normalAttr = geometry.attributes.normal;
	const tangentAttr = geometry.attributes.tangent;

	const tOut = new Vector3().fromBufferAttribute( tangentAttr, vertexIndex );
	const wOut = tangentAttr.getW( vertexIndex );
	const nOut = new Vector3().fromBufferAttribute( normalAttr, vertexIndex );
	const nLen = nOut.length();
	nOut.divideScalar( nLen );

	// structural guarantees on the output frame
	assert.ok(
		Number.isFinite( tOut.x ) && Number.isFinite( tOut.y ) && Number.isFinite( tOut.z ),
		`${ label }: tangent xyz is finite`,
	);
	assert.ok( Math.abs( tOut.length() - 1 ) < EPS, `${ label }: tangent is unit length` );
	assert.ok( Math.abs( nLen - 1 ) < 1e-4, `${ label }: normal is unit length` );
	assert.ok( Math.abs( tOut.dot( nOut ) ) < EPS, `${ label }: tangent is orthogonal to the normal` );
	assert.ok( wOut === 1 || wOut === - 1, `${ label }: tangent w is exactly ±1` );

	// the frame matches the independently computed expectation
	assertVectorNear( tOut, expected.T, EPS, `${ label }: tangent direction` );
	assertVectorNear( nOut, expected.N, EPS, `${ label }: normal direction` );
	assert.equal( wOut, expected.w, `${ label }: bitangent sign` );

	// a fixed tangent-space normal reconstructs to the same direction
	const actual = reconstructTangentSpaceNormal( tOut, nOut, wOut );
	const wanted = reconstructTangentSpaceNormal( expected.T, expected.N, expected.w );
	assertVectorNear( actual, wanted, EPS, `${ label }: reconstructed tangent-space normal` );

}

// verifies every output vertex of a result brush against the source data
function verifyResult( result, brushA, brushB, options = {} ) {

	const {
		isSurfaceInverted = () => false,
		plainAttributes = [ 'uv' ],
	} = options;

	const geometry = result.geometry;
	const posAttr = geometry.attributes.position;
	const tanAttr = geometry.attributes.tangent;

	assert.ok( geometry.index, 'result stays indexed' );
	assert.ok( tanAttr, 'tangent attribute exists' );
	assert.equal( tanAttr.itemSize, 4, 'tangent is a four component attribute' );
	assert.equal( tanAttr.count, posAttr.count, 'tangent and position counts match' );

	// every indexed tangent is finite with w = ±1
	const index = geometry.index;
	for ( let i = 0; i < index.count; i ++ ) {

		const vi = index.getX( i );
		assert.ok(
			Number.isFinite( tanAttr.getX( vi ) ) &&
			Number.isFinite( tanAttr.getY( vi ) ) &&
			Number.isFinite( tanAttr.getZ( vi ) ) &&
			Number.isFinite( tanAttr.getW( vi ) ),
			`indexed tangent ${ vi } is finite`,
		);
		const w = tanAttr.getW( vi );
		assert.ok( w === 1 || w === - 1, `indexed tangent ${ vi } has w = ±1` );

	}

	// the result geometry lives in brush A's local frame
	const relB = new Matrix4().copy( brushA.matrixWorld ).invert().multiply( brushB.matrixWorld );
	const sources = [
		{ geometry: brushA.geometry, matrix: null },
		{ geometry: brushB.geometry, matrix: relB },
	];
	const relMatrices = [ new Matrix4(), relB ];

	const stats = { a: 0, b: 0, newVertices: 0 };
	const p = [ new Vector3(), new Vector3(), new Vector3() ];
	const centroid = new Vector3();
	const triCount = index.count / 3;
	for ( let t = 0; t < triCount; t ++ ) {

		const ids = [ index.getX( 3 * t ), index.getX( 3 * t + 1 ), index.getX( 3 * t + 2 ) ];
		centroid.set( 0, 0, 0 );
		for ( let k = 0; k < 3; k ++ ) {

			p[ k ].fromBufferAttribute( posAttr, ids[ k ] );
			centroid.add( p[ k ] );

		}

		centroid.divideScalar( 3 );

		// find the source triangle this output triangle was cut from
		const src = findSourceTriangle( centroid, sources );
		assert.ok( src, `output triangle ${ t } has a source triangle` );
		stats[ src.source === 0 ? 'a' : 'b' ] ++;

		const source = sources[ src.source ];
		const surfaceInverted = isSurfaceInverted( src.source );
		const verts = readTriVerts( source.geometry, src.triIndex, source.matrix );
		_tri.a.copy( verts[ 0 ] );
		_tri.b.copy( verts[ 1 ] );
		_tri.c.copy( verts[ 2 ] );

		for ( let k = 0; k < 3; k ++ ) {

			_tri.getBarycoord( p[ k ], _bary );
			const expected = expectedFrame(
				source.geometry,
				src.triIndex,
				_bary,
				relMatrices[ src.source ],
				surfaceInverted,
			);
			verifyVertexFrame( geometry, ids[ k ], expected, `triangle ${ t } vertex ${ k }` );

			if ( _bary.x < 1 - 1e-6 && _bary.y < 1 - 1e-6 && _bary.z < 1 - 1e-6 ) {

				stats.newVertices ++;

			}

			// non-directional attributes are interpolated as-is
			for ( const name of plainAttributes ) {

				const outAttr = geometry.attributes[ name ];
				const srcAttr = source.geometry.attributes[ name ];
				const weights = [ _bary.x, _bary.y, _bary.z ];
				for ( let c = 0; c < outAttr.itemSize; c ++ ) {

					let wanted = 0;
					for ( let j = 0; j < 3; j ++ ) {

						const si = source.geometry.index.getX( 3 * src.triIndex + j );
						wanted += srcAttr.array[ si * srcAttr.itemSize + c ] * weights[ j ];

					}

					const actual = outAttr.array[ ids[ k ] * outAttr.itemSize + c ];
					assert.ok(
						Math.abs( actual - wanted ) < EPS,
						`triangle ${ t } vertex ${ k }: ${ name } is interpolated without transformation`,
					);

				}

			}

		}

	}

	return stats;

}

test( 'tangent-frame fixture satisfies the tangent frame convention', () => {

	for ( const mirrored of [ MIRRORED_FACES_A, MIRRORED_FACES_B ] ) {

		const geometry = makeGeometry( mirrored );
		const tangent = geometry.attributes.tangent;
		const normal = geometry.attributes.normal;
		const index = geometry.index;
		assert.equal( tangent.itemSize, 4 );

		const signs = new Set();
		for ( let t = 0; t < index.count / 3; t ++ ) {

			const ws = [ 0, 1, 2 ].map( k => tangent.getW( index.getX( 3 * t + k ) ) );
			assert.ok( ws.every( w => w === ws[ 0 ] ), 'source triangle has a uniform w' );
			signs.add( ws[ 0 ] );

		}

		assert.ok( signs.has( 1 ) && signs.has( - 1 ), 'both bitangent signs are present' );

		for ( let i = 0; i < tangent.count; i ++ ) {

			const tv = new Vector3().fromBufferAttribute( tangent, i );
			const nv = new Vector3().fromBufferAttribute( normal, i );
			assert.ok( Number.isFinite( tv.x ) && Number.isFinite( tv.y ) && Number.isFinite( tv.z ) );
			assert.ok( Math.abs( tv.length() - 1 ) < EPS, 'source tangent is unit length' );
			assert.ok( Math.abs( tv.dot( nv ) ) < EPS, 'source tangent is orthogonal to the normal' );
			const w = tangent.getW( i );
			assert.ok( w === 1 || w === - 1, 'source tangent w is exactly ±1' );

		}

	}

} );

test( 'tangent-frame union under rotation and non-uniform scale', () => {

	const { a, b } = makeIntersectingPair();
	const evaluator = new Evaluator();
	evaluator.attributes = [ 'position', 'normal', 'uv', 'tangent', 'color' ];
	const result = evaluator.evaluate( a, b, ADDITION );

	const stats = verifyResult( result, a, b, { plainAttributes: [ 'uv', 'color' ] } );
	assert.ok( stats.a > 0, 'triangles carried over from brush A' );
	assert.ok( stats.b > 0, 'triangles carried over from brush B' );
	assert.ok( stats.newVertices > 0, 'cut vertices were generated' );

} );

test( 'tangent-frame union under reflection transform', () => {

	const a = makeBrush( MIRRORED_FACES_A, {
		position: [ 0.1, - 0.2, 0.05 ],
		rotation: [ 0.2, 0.4, - 0.3 ],
		scale: [ 1.2, 0.9, 1.1 ],
	} );
	const b = makeBrush( MIRRORED_FACES_B, {
		position: [ 0.4, 0.3, - 0.2 ],
		rotation: [ 0.3, - 0.6, 0.2 ],
		scale: [ - 0.9, 1.1, 0.85 ],
	} );

	// make sure the relative transform really contains a reflection
	const relB = new Matrix4().copy( a.matrixWorld ).invert().multiply( b.matrixWorld );
	assert.ok( relB.determinant() < 0, 'relative transform is a reflection' );

	const evaluator = new Evaluator();
	evaluator.attributes = [ 'position', 'normal', 'uv', 'tangent' ];
	const result = evaluator.evaluate( a, b, ADDITION );

	const stats = verifyResult( result, a, b );
	assert.ok( stats.a > 0 && stats.b > 0, 'triangles carried over from both brushes' );
	assert.ok( stats.newVertices > 0, 'cut vertices were generated' );

} );

test( 'tangent-frame subtraction inner walls flip normal and w only', () => {

	const { a, b } = makeIntersectingPair();
	const evaluator = new Evaluator();
	evaluator.attributes = [ 'position', 'normal', 'uv', 'tangent' ];
	const result = evaluator.evaluate( a, b, SUBTRACTION );

	// the parts of brush B's surface kept by subtraction are inner walls: the
	// surface is inverted, so the normal and w flip while the tangent direction
	// along U is preserved
	const stats = verifyResult( result, a, b, { isSurfaceInverted: source => source === 1 } );
	assert.ok( stats.a > 0, 'outer surface from brush A is present' );
	assert.ok( stats.b > 0, 'inverted inner wall triangles from brush B are present' );
	assert.ok( stats.newVertices > 0, 'cut vertices were generated' );

} );

test( 'tangent-frame intersection and disjoint brushes', () => {

	const { a, b } = makeIntersectingPair();
	const evaluator = new Evaluator();
	evaluator.attributes = [ 'position', 'normal', 'uv', 'tangent' ];

	const intersection = evaluator.evaluate( a, b, INTERSECTION );
	const stats = verifyResult( intersection, a, b );
	assert.ok( stats.a > 0 && stats.b > 0, 'intersection contains surface from both brushes' );
	assert.ok( stats.newVertices > 0, 'cut vertices were generated' );

	// fully separated brushes produce only whole, untouched triangles
	const c = makeBrush( MIRRORED_FACES_A );
	const d = makeBrush( MIRRORED_FACES_B, { position: [ 5, 0, 0 ] } );

	const union = evaluator.evaluate( c, d, ADDITION );
	const unionStats = verifyResult( union, c, d );
	assert.ok( unionStats.a > 0 && unionStats.b > 0, 'both brushes are carried over whole' );
	assert.equal( unionStats.newVertices, 0, 'no cut vertices for disjoint brushes' );

	const difference = evaluator.evaluate( c, d, SUBTRACTION );
	const diffStats = verifyResult( difference, c, d, { isSurfaceInverted: source => source === 1 } );
	assert.ok( diffStats.a > 0, 'brush A remains' );
	assert.equal( diffStats.b, 0, 'disjoint subtraction keeps nothing of brush B' );

} );

test( 'tangent-frame batch operations and material identity', () => {

	const { a, b } = makeIntersectingPair();
	a.material = new MeshBasicMaterial();
	b.material = new MeshBasicMaterial();
	const matA = a.material;
	const matB = b.material;

	const evaluator = new Evaluator();
	evaluator.attributes = [ 'position', 'normal', 'uv', 'tangent' ];
	const unionTarget = new Brush();
	const intersectionTarget = new Brush();
	const results = evaluator.evaluate( a, b, [ ADDITION, INTERSECTION ], [ unionTarget, intersectionTarget ] );

	assert.equal( results[ 0 ], unionTarget, 'batch results use the provided targets' );
	assert.equal( results[ 1 ], intersectionTarget );

	verifyResult( unionTarget, a, b );
	verifyResult( intersectionTarget, a, b );

	// material objects keep their identity
	assert.ok( unionTarget.material.includes( matA ), 'brush A material is reused' );
	assert.ok( unionTarget.material.includes( matB ), 'brush B material is reused' );

} );

test( 'tangent-frame target reuse and evaluator option switches', () => {

	const { a, b } = makeIntersectingPair();
	const evaluator = new Evaluator();
	evaluator.attributes = [ 'position', 'normal', 'uv', 'tangent' ];
	const target = new Brush();

	evaluator.evaluate( a, b, SUBTRACTION, target );
	verifyResult( target, a, b, { isSurfaceInverted: source => source === 1 } );

	// reuse the same target brush for another operation
	evaluator.evaluate( a, b, ADDITION, target );
	verifyResult( target, a, b );

	// narrowing the attribute list must not leave stale tangent data behind
	evaluator.attributes = [ 'position', 'normal', 'uv' ];
	evaluator.evaluate( a, b, ADDITION, target );
	assert.ok( ! target.geometry.attributes.tangent, 'tangent attribute is removed' );
	assert.equal(
		target.geometry.attributes.normal.count,
		target.geometry.attributes.position.count,
		'remaining attributes stay aligned',
	);

	// restoring the attribute list must reproduce valid frames
	evaluator.attributes = [ 'position', 'normal', 'uv', 'tangent' ];
	evaluator.evaluate( a, b, SUBTRACTION, target );
	verifyResult( target, a, b, { isSurfaceInverted: source => source === 1 } );

	// group handling switches must not disturb the vertex data
	evaluator.useGroups = false;
	evaluator.evaluate( a, b, ADDITION, target );
	verifyResult( target, a, b );

	evaluator.useGroups = true;
	evaluator.consolidateGroups = false;
	evaluator.evaluate( a, b, ADDITION, target );
	verifyResult( target, a, b );

} );
