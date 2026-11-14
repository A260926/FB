import { BufferGeometry } from '../src/core/BufferGeometry.js';
import {
	Float32BufferAttribute,
	Uint16BufferAttribute,
	Uint8BufferAttribute,
	Int16BufferAttribute
} from '../src/core/BufferAttribute.js';
import { InterleavedBuffer } from '../src/core/InterleavedBuffer.js';
import { InterleavedBufferAttribute } from '../src/core/InterleavedBufferAttribute.js';
import { Bone } from '../src/objects/Bone.js';
import { Skeleton } from '../src/objects/Skeleton.js';
import { SkinnedMesh } from '../src/objects/SkinnedMesh.js';
import { Matrix4 } from '../src/math/Matrix4.js';
import { Vector3 } from '../src/math/Vector3.js';
import { AnimationMixer } from '../src/animation/AnimationMixer.js';
import { AnimationClip } from '../src/animation/AnimationClip.js';
import { NumberKeyframeTrack } from '../src/animation/tracks/NumberKeyframeTrack.js';
import { DetachedBindMode } from '../src/constants.js';
import { MeshBasicMaterial } from '../src/materials/MeshBasicMaterial.js';
import { partitionSkinnedMesh } from '../src/objects/SkinPalettePartitioner.js';

function arrayEqual( a, b ) {

	if ( a.length !== b.length ) return false;

	for ( let i = 0; i < a.length; i ++ ) if ( a[ i ] !== b[ i ] ) return false;

	return true;

}

function checkArray( check, actual, expected, message ) {

	check( arrayEqual( Array.from( actual ), expected ), message );

}

// Builds the shared packing fixture: four bones and four triangles.
// T0 (0,1,2) bones {0,1}; T1 (2,3,4) bones {1,2};
// T2 (4,5,6) bones {2,3}; T3 (6,7,0) bones {3,0}.
// Zero-weight slots carry fake bone indices that must never occupy a slot.

function buildPackingMesh() {

	const bones = [ new Bone(), new Bone(), new Bone(), new Bone() ];

	bones[ 0 ].add( bones[ 1 ], bones[ 3 ] );
	bones[ 1 ].add( bones[ 2 ] );
	bones[ 0 ].updateMatrixWorld( true );

	const skeleton = new Skeleton( bones );

	const geometry = new BufferGeometry();

	geometry.setAttribute( 'position', new Float32BufferAttribute( [
		0, 0, 0,
		1, 0, 0,
		1, 1, 0,
		0, 2, 0,
		2, 1, 0,
		2, 2, 0,
		3, 1, 0,
		3, 0, 0
	], 3 ) );

	geometry.setAttribute( 'skinIndex', new Uint16BufferAttribute( [
		0, 99, 99, 99, // v0 -> bone 0, fake indices in zero slots
		1, 99, 99, 99, // v1 -> bone 1
		1, 99, 99, 99, // v2 -> bone 1 (shared by T0 and T1)
		1, 2, 99, 99, // v3 -> bones 1,2
		2, 99, 99, 99, // v4 -> bone 2
		2, 3, 99, 99, // v5 -> bones 2,3
		3, 99, 99, 99, // v6 -> bone 3
		3, 0, 88, 99 // v7 -> bones 3,0
	], 4 ) );

	geometry.setAttribute( 'skinWeight', new Float32BufferAttribute( [
		1, 0, 0, 0,
		1, 0, 0, 0,
		1, 0, 0, 0,
		0.5, 0.5, 0, 0,
		1, 0, 0, 0,
		0.5, 0.5, 0, 0,
		1, 0, 0, 0,
		0.5, 0.5, 0, 0
	], 4 ) );

	geometry.setIndex( [ 0, 1, 2, 2, 3, 4, 4, 5, 6, 6, 7, 0 ] );
	geometry.addGroup( 0, 6, 0 );
	geometry.addGroup( 6, 6, 1 );

	const materialA = new MeshBasicMaterial();
	const materialB = new MeshBasicMaterial();
	const mesh = new SkinnedMesh( geometry, [ materialA, materialB ] );
	mesh.bind( skeleton );

	return { mesh, skeleton, bones, materials: [ materialA, materialB ] };

}

const packing = ( check ) => {

	const { mesh, materials } = buildPackingMesh();

	// maxBones validation.

	for ( const bad of [ 0, - 1, 1.5, NaN, Infinity, '3', undefined ] ) {

		let threw = false;

		try {

			partitionSkinnedMesh( mesh, bad );

		} catch ( error ) {

			threw = true;

		}

		check( threw, `maxBones ${ bad } must be rejected` );

	}

	// A single triangle exceeding the palette must raise RangeError.

	const wideGeometry = new BufferGeometry();

	wideGeometry.setAttribute( 'position', new Float32BufferAttribute( [ 0, 0, 0, 1, 0, 0, 0, 1, 0 ], 3 ) );
	wideGeometry.setAttribute( 'skinIndex', new Uint16BufferAttribute( [
		0, 99, 99, 99,
		1, 99, 99, 99,
		2, 99, 99, 99
	], 4 ) );
	wideGeometry.setAttribute( 'skinWeight', new Float32BufferAttribute( [
		1, 0, 0, 0,
		1, 0, 0, 0,
		1, 0, 0, 0
	], 4 ) );
	wideGeometry.setIndex( [ 0, 1, 2 ] );

	const wideMesh = new SkinnedMesh( wideGeometry, new MeshBasicMaterial() );
	wideMesh.bind( new Skeleton( [ new Bone(), new Bone(), new Bone() ] ) );

	let rangeError = false;

	try {

		partitionSkinnedMesh( wideMesh, 2 );

	} catch ( error ) {

		rangeError = error instanceof RangeError;

	}

	check( rangeError, 'oversized single triangle throws RangeError' );

	// Greedy packing with capacity 2: every pack fills the palette exactly and
	// the next triangle forces a new pack.

	const packs = partitionSkinnedMesh( mesh, 2 );

	check( packs.length === 4, 'four packs at maxBones 2' );
	checkArray( check, packs[ 0 ].boneIndices, [ 0, 1 ], 'pack 0 palette' );
	checkArray( check, packs[ 1 ].boneIndices, [ 1, 2 ], 'pack 1 palette' );
	checkArray( check, packs[ 2 ].boneIndices, [ 2, 3 ], 'pack 2 palette' );
	checkArray( check, packs[ 3 ].boneIndices, [ 0, 3 ], 'pack 3 palette sorted ascending' );

	for ( const pack of packs ) {

		check( pack.boneIndices.length <= 2, 'palette within capacity' );

		for ( let i = 1; i < pack.boneIndices.length; i ++ ) {

			check( pack.boneIndices[ i - 1 ] < pack.boneIndices[ i ], 'bone indices sorted' );

		}

	}

	// Triangle mapping: contiguous, complete, in input order.

	const flatTriangles = packs.flatMap( pack => pack.sourceTriangleIndices );
	checkArray( check, flatTriangles, [ 0, 1, 2, 3 ], 'all triangles covered in order' );

	for ( let p = 0; p < packs.length; p ++ ) {

		const triangles = packs[ p ].sourceTriangleIndices;

		for ( let i = 1; i < triangles.length; i ++ ) {

			check( triangles[ i ] === triangles[ i - 1 ] + 1, `pack ${ p } triangles contiguous` );

		}

	}

	// Cross-pack shared vertex 2 appears in packs 0 and 1, duplicated; within a
	// pack the shared vertex is reused through the local index.

	check( packs[ 0 ].sourceVertexIndices.includes( 2 ), 'vertex 2 in pack 0' );
	check( packs[ 1 ].sourceVertexIndices.includes( 2 ), 'vertex 2 duplicated into pack 1' );
	checkArray( check, packs[ 0 ].sourceVertexIndices, [ 0, 1, 2 ], 'first-encounter vertex order pack 0' );
	checkArray( check, packs[ 1 ].sourceVertexIndices, [ 2, 3, 4 ], 'first-encounter vertex order pack 1' );

	const pack0Index = Array.from( packs[ 0 ].mesh.geometry.index.array );
	checkArray( check, pack0Index, [ 0, 1, 2 ], 'pack 0 local index reuses shared vertex' );
	check( new Set( pack0Index ).size === 3, 'three distinct local vertices' );

	// Local index maps back to the source triangle through sourceVertexIndices.

	for ( const pack of packs ) {

		const localIndex = Array.from( pack.mesh.geometry.index.array );
		const sourceIndex = mesh.geometry.getIndex();

		for ( let tri = 0; tri < pack.sourceTriangleIndices.length; tri ++ ) {

			const sourceTriangle = pack.sourceTriangleIndices[ tri ];

			for ( let corner = 0; corner < 3; corner ++ ) {

				const localVertex = localIndex[ tri * 3 + corner ];
				const expectedSourceVertex = sourceIndex.getX( sourceTriangle * 3 + corner );

				check(
					pack.sourceVertexIndices[ localVertex ] === expectedSourceVertex,
					'local index round-trips to source triangle vertex'
				);

			}

		}

	}

	// Zero-weight fake indices never occupy palette capacity and are rewritten
	// to local slot 0; positive slots become local palette numbers; weights stay.

	const pack3 = packs[ 3 ];
	const localSkinIndex = pack3.mesh.geometry.getAttribute( 'skinIndex' );
	const localSkinWeight = pack3.mesh.geometry.getAttribute( 'skinWeight' );
	const sourceSkinIndex = mesh.geometry.getAttribute( 'skinIndex' );
	const sourceSkinWeight = mesh.geometry.getAttribute( 'skinWeight' );

	for ( let local = 0; local < pack3.sourceVertexIndices.length; local ++ ) {

		const sourceVertex = pack3.sourceVertexIndices[ local ];

		for ( let slot = 0; slot < 4; slot ++ ) {

			const weight = sourceSkinWeight.getComponent( sourceVertex, slot );
			check( localSkinWeight.getComponent( local, slot ) === weight, 'weight preserved' );

			if ( weight === 0 ) {

				check( localSkinIndex.getComponent( local, slot ) === 0, 'zero-weight slot rewritten to 0' );

			} else {

				const sourceBone = sourceSkinIndex.getComponent( sourceVertex, slot );
				const expectedLocal = pack3.boneIndices.indexOf( sourceBone );

				check( localSkinIndex.getComponent( local, slot ) === expectedLocal, 'positive slot uses local bone number' );

			}

		}

	}

	// Groups keep the source material index of every source triangle.

	for ( const pack of packs ) {

		const groups = pack.mesh.geometry.groups;
		const covered = [];

		for ( const group of groups ) {

			for ( let offset = group.start; offset < group.start + group.count; offset += 3 ) {

				const localTriangle = offset / 3;
				const sourceTriangle = pack.sourceTriangleIndices[ localTriangle ];
				const sourceGroup = mesh.geometry.groups.find( g => sourceTriangle * 3 >= g.start && sourceTriangle * 3 < g.start + g.count );

				check( group.materialIndex === sourceGroup.materialIndex, 'group material index preserved' );
				covered.push( sourceTriangle );

			}

		}

		checkArray( check, covered, pack.sourceTriangleIndices, 'groups cover all pack triangles' );

	}

	check( packs[ 0 ].mesh.material === mesh.material, 'material array identity shared' );
	check( packs[ 0 ].mesh.material[ 0 ] === materials[ 0 ], 'material object identity shared' );

	// At capacity 3 the greedy run can keep adding: T0+T1 share palette
	// {0,1,2} exactly full, T2 forces a split, then T3 joins the second pack.

	const packs3 = partitionSkinnedMesh( mesh, 3 );

	check( packs3.length === 2, 'two packs at maxBones 3' );
	checkArray( check, packs3[ 0 ].sourceTriangleIndices, [ 0, 1 ], 'greedy continues while fitting' );
	checkArray( check, packs3[ 1 ].sourceTriangleIndices, [ 2, 3 ], 'split only when overflowing' );
	check( packs3[ 0 ].boneIndices.length === 3, 'capacity exhausted exactly' );
	checkArray( check, packs3[ 1 ].boneIndices, [ 0, 2, 3 ], 'second pack palette' );

	// Vertex 2 shared inside pack 0 is stored once and referenced twice.

	checkArray( check, packs3[ 0 ].sourceVertexIndices, [ 0, 1, 2, 3, 4 ], 'shared vertex stored once per pack' );

	const pack30Index = Array.from( packs3[ 0 ].mesh.geometry.index.array );
	check( pack30Index[ 2 ] === pack30Index[ 3 ], 'shared vertex reused via local index' );

};

const attributes = ( check ) => {

	// Interleaved position+normal, separate normalized integer color and a
	// custom integer attribute, plus two morph channels.

	const geometry = new BufferGeometry();

	const interleaved = new InterleavedBuffer( new Float32Array( [
		0, 0, 0, 1, 0, 0,
		1, 0, 0, 1, 0, 0,
		1, 1, 0, 0, 1, 0,
		0, 1, 0, 0, 1, 0,
		2, 2, 0, 0, 0, 1,
		2, 0, 0, 0, 0, 1
	] ), 6 );

	geometry.setAttribute( 'position', new InterleavedBufferAttribute( interleaved, 3, 0 ) );
	geometry.setAttribute( 'normal', new InterleavedBufferAttribute( interleaved, 3, 3 ) );
	geometry.setAttribute( 'uv', new Float32BufferAttribute( [
		0, 0, 1, 0, 1, 1, 0, 1, 2, 2, 2, 0
	], 2 ) );
	geometry.setAttribute( 'color', new Uint8BufferAttribute( [
		255, 0, 127,
		0, 255, 63,
		1, 2, 3,
		4, 5, 6,
		200, 100, 50,
		9, 8, 7
	], 3, true ) );
	geometry.setAttribute( 'intensity', new Int16BufferAttribute( [ 10, 20, 30, 40, 50, 60 ], 1 ) );

	geometry.setAttribute( 'skinIndex', new Uint16BufferAttribute( [
		0, 99, 99, 99, // v0 -> bone 0 (shared by T0 and T2)
		0, 1, 99, 99, // v1 -> bones 0,1
		1, 99, 99, 99, // v2 -> bone 1 (shared by T0 and T1)
		1, 2, 99, 99, // v3 -> bones 1,2
		2, 99, 99, 99, // v4 -> bone 2 (shared by T1 and T2)
		2, 0, 99, 99 // v5 -> bones 2,0
	], 4 ) );
	geometry.setAttribute( 'skinWeight', new Float32BufferAttribute( [
		1, 0, 0, 0,
		0.5, 0.5, 0, 0,
		1, 0, 0, 0,
		0.5, 0.5, 0, 0,
		1, 0, 0, 0,
		0.5, 0.5, 0, 0
	], 4 ) );

	geometry.setIndex( [ 0, 1, 2, 2, 3, 4, 4, 5, 0 ] );

	geometry.morphAttributes.position = [
		new Float32BufferAttribute( [
			0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1, 0
		], 3 ),
		new Float32BufferAttribute( [
			1, 0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0
		], 3 )
	];
	geometry.morphTargetsRelative = true;

	const skeleton = new Skeleton( [ new Bone(), new Bone(), new Bone() ] );
	const mesh = new SkinnedMesh( geometry, new MeshBasicMaterial() );
	mesh.bind( skeleton );

	const packs = partitionSkinnedMesh( mesh, 2 );

	check( packs.length >= 2, 'multiple packs produced' );

	for ( const pack of packs ) {

		const out = pack.mesh.geometry;

		check( out.index !== null, 'output geometry is indexed' );

		for ( const name of [ 'position', 'normal', 'uv', 'color', 'intensity' ] ) {

			const source = geometry.getAttribute( name );
			const target = out.getAttribute( name );

			check( target.isBufferAttribute === true, `${ name } de-interleaved to plain attribute` );
			check( target.array.constructor === source.array.constructor, `${ name } keeps typed-array type` );
			check( target.itemSize === source.itemSize, `${ name } keeps itemSize` );
			check( target.normalized === source.normalized, `${ name } keeps normalized flag` );
			check( target.array.buffer !== source.array.buffer, `${ name } storage copied` );

			for ( let local = 0; local < pack.sourceVertexIndices.length; local ++ ) {

				const sourceVertex = pack.sourceVertexIndices[ local ];

				for ( let component = 0; component < source.itemSize; component ++ ) {

					check(
						target.array[ local * target.itemSize + component ] === source.array[ ( source.isInterleavedBufferAttribute
							? sourceVertex * source.data.stride + source.offset
							: sourceVertex * source.itemSize ) + component ],
						`${ name } raw value remapped`
					);

				}

				check(
					target.getComponent( local, 0 ) === source.getComponent( sourceVertex, 0 ),
					`${ name } normalized readback matches`
				);

			}

		}

		check( out.morphTargetsRelative === true, 'morphTargetsRelative preserved' );
		check( out.morphAttributes.position.length === 2, 'both morph channels retained' );

		for ( let channel = 0; channel < 2; channel ++ ) {

			const sourceMorph = geometry.morphAttributes.position[ channel ];
			const targetMorph = out.morphAttributes.position[ channel ];

			check( targetMorph.array.constructor === Float32Array, 'morph keeps Float32 type' );
			check( targetMorph.array.buffer !== sourceMorph.array.buffer, 'morph storage copied' );

			for ( let local = 0; local < pack.sourceVertexIndices.length; local ++ ) {

				const sourceVertex = pack.sourceVertexIndices[ local ];

				for ( let component = 0; component < 3; component ++ ) {

					check(
						targetMorph.array[ local * 3 + component ] === sourceMorph.array[ sourceVertex * 3 + component ],
						'morph channel values remapped'
					);

				}

			}

		}

	}

	// Non-indexed input: triangles count per three vertices, output indexed
	// with first-encounter vertex order.

	const nonIndexed = new BufferGeometry();

	nonIndexed.setAttribute( 'position', new Float32BufferAttribute( [
		0, 0, 0, 1, 0, 0, 0, 1, 0,
		2, 0, 0, 3, 0, 0, 2, 1, 0
	], 3 ) );
	nonIndexed.setAttribute( 'skinIndex', new Uint16BufferAttribute( [
		0, 99, 99, 99,
		0, 99, 99, 99,
		1, 99, 99, 99,
		2, 99, 99, 99,
		2, 99, 99, 99,
		3, 99, 99, 99
	], 4 ) );
	nonIndexed.setAttribute( 'skinWeight', new Float32BufferAttribute( [
		1, 0, 0, 0,
		1, 0, 0, 0,
		1, 0, 0, 0,
		1, 0, 0, 0,
		1, 0, 0, 0,
		1, 0, 0, 0
	], 4 ) );

	const soupMesh = new SkinnedMesh( nonIndexed, new MeshBasicMaterial() );
	soupMesh.bind( new Skeleton( [ new Bone(), new Bone(), new Bone(), new Bone() ] ) );

	const soupPacks = partitionSkinnedMesh( soupMesh, 2 );

	check( soupPacks.length === 2, 'non-indexed geometry split by three-vertex triangles' );
	checkArray( check, soupPacks[ 0 ].sourceTriangleIndices, [ 0 ], 'first soup triangle' );
	checkArray( check, soupPacks[ 1 ].sourceTriangleIndices, [ 1 ], 'second soup triangle' );
	checkArray( check, soupPacks[ 0 ].sourceVertexIndices, [ 0, 1, 2 ], 'soup first-encounter order' );
	checkArray( check, soupPacks[ 1 ].sourceVertexIndices, [ 3, 4, 5 ], 'soup vertices copied across packs' );
	check( soupPacks[ 0 ].mesh.geometry.index !== null, 'soup output indexed' );

};

function buildDeformMesh( relative ) {

	const root = new Bone();
	root.name = 'root';
	root.position.set( 0, 1, 0 );
	root.scale.set( 1.5, 2, 0.7 ); // non-uniform scale on shared ancestor

	const child = new Bone();
	child.name = 'child';
	child.position.set( 1, 0, 0 );
	child.scale.set( 2, 0.5, 3 ); // non-uniform scale on animated bone

	const other = new Bone();
	other.name = 'other';
	other.position.set( 0, 0, 1 );

	root.add( child, other );
	root.updateMatrixWorld( true );

	const skeleton = new Skeleton( [ root, child, other ] );

	const geometry = new BufferGeometry();

	geometry.setAttribute( 'position', new Float32BufferAttribute( [
		0, 0, 0, // v0 bone 0
		1, 0, 0, // v1 bone 1
		1, 1, 0, // v2 bone 1
		0, 2, 0, // v3 bones 1,2
		2, 1, 0 // v4 bone 2
	], 3 ) );

	geometry.setAttribute( 'skinIndex', new Uint16BufferAttribute( [
		0, 99, 99, 99,
		1, 99, 99, 99,
		1, 99, 99, 99,
		1, 2, 77, 99,
		2, 99, 99, 99
	], 4 ) );

	geometry.setAttribute( 'skinWeight', new Float32BufferAttribute( [
		1, 0, 0, 0,
		1, 0, 0, 0,
		1, 0, 0, 0,
		0.5, 0.5, 0, 0,
		1, 0, 0, 0
	], 4 ) );

	// T0 (0,1,2) -> {0,1}; T1 (2,3,4) -> {1,2}
	geometry.setIndex( [ 0, 1, 2, 2, 3, 4 ] );

	const basePositions = [ 0, 0, 0, 1, 0, 0, 1, 1, 0, 0, 2, 0, 2, 1, 0 ];

	if ( relative ) {

		geometry.morphAttributes.position = [ new Float32BufferAttribute( [
			0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1, 0
		], 3 ) ];

	} else {

		geometry.morphAttributes.position = [ new Float32BufferAttribute(
			basePositions.map( ( value, i ) => i % 3 === 1 ? value + 1 : value ),
			3
		) ];

	}

	geometry.morphTargetsRelative = relative;

	const mesh = new SkinnedMesh( geometry, new MeshBasicMaterial() );

	// Non-identity detached bind transform.
	const bind = new Matrix4().makeRotationZ( Math.PI / 4 );
	bind.setPosition( 2, - 1, 0.5 );
	mesh.bind( skeleton, bind );
	mesh.bindMode = DetachedBindMode;
	mesh.updateMatrixWorld( true );

	return { mesh, root, child, other, skeleton };

}

function compareDeformation( check, mesh, packs, stage ) {

	const expected = new Vector3();
	const actual = new Vector3();

	for ( const pack of packs ) {

		pack.mesh.skeleton.update();

		for ( let local = 0; local < pack.sourceVertexIndices.length; local ++ ) {

			const sourceVertex = pack.sourceVertexIndices[ local ];

			mesh.getVertexPosition( sourceVertex, expected );
			pack.mesh.getVertexPosition( local, actual );

			const tolerance = 1e-5 * ( 1 + expected.length() );

			check(
				expected.distanceTo( actual ) <= tolerance,
				`deformation matches at ${ stage } source vertex ${ sourceVertex }`
			);

		}

	}

}

const deformation = ( check ) => {

	for ( const relative of [ true, false ] ) {

		const mode = relative ? 'relative' : 'absolute';
		const { mesh, root, skeleton } = buildDeformMesh( relative );

		for ( const maxBones of [ 2, 3 ] ) {

			const packs = partitionSkinnedMesh( mesh, maxBones );

			check( packs[ 0 ].mesh.isSkinnedMesh === true, 'pack is a SkinnedMesh' );
			check( packs[ 0 ].mesh.bindMode === DetachedBindMode, 'bindMode copied' );
			check( packs[ 0 ].mesh.bindMatrix.equals( mesh.bindMatrix ), 'bindMatrix copied' );
			check( packs[ 0 ].mesh.bindMatrixInverse.equals( mesh.bindMatrixInverse ), 'bindMatrixInverse copied' );

			// Bone identity shared, inverse bind matrices copied.
			for ( const pack of packs ) {

				for ( let local = 0; local < pack.boneIndices.length; local ++ ) {

					const sourceBoneIndex = pack.boneIndices[ local ];

					check(
						pack.mesh.skeleton.bones[ local ] === skeleton.bones[ sourceBoneIndex ],
						`${ mode } pack reuses source Bone object`
					);
					check(
						pack.mesh.skeleton.boneInverses[ local ] !== skeleton.boneInverses[ sourceBoneIndex ],
						`${ mode } inverse bind matrix is a copy`
					);
					check(
						pack.mesh.skeleton.boneInverses[ local ].equals( skeleton.boneInverses[ sourceBoneIndex ] ),
						`${ mode } inverse bind matrix values match`
					);

				}

			}

			// Rest pose.
			compareDeformation( check, mesh, packs, `${ mode } rest maxBones ${ maxBones }` );

			// Animation drives the shared bone hierarchy; packs follow automatically.
			const mixer = new AnimationMixer( root );
			mixer.clipAction( new AnimationClip( 'move', 1, [
				new NumberKeyframeTrack( '.position[x]', [ 0, 1 ], [ 0, 4 ] )
			] ) ).play();
			mixer.update( 0.5 );
			root.updateMatrixWorld( true );
			skeleton.update();

			check( Math.abs( root.position.x - 2 ) < 1e-9, 'animation advanced' );
			compareDeformation( check, mesh, packs, `${ mode } animated maxBones ${ maxBones }` );

			// Morph weights applied on both source and packs.
			mesh.morphTargetInfluences[ 0 ] = 0.5;

			for ( const pack of packs ) pack.mesh.morphTargetInfluences[ 0 ] = 0.5;

			compareDeformation( check, mesh, packs, `${ mode } animated+morph maxBones ${ maxBones }` );

			mixer.stopAllAction();
			root.position.x = 0;
			root.updateMatrixWorld( true );
			skeleton.update();

		}

	}

	// Attached bind mode with a non-identity mesh world transform.

	const root = new Bone();
	root.position.set( 0, 2, 0 );

	const child = new Bone();
	child.position.set( 0, 1, 0 );
	root.add( child );
	root.updateMatrixWorld( true );

	const attachedSkeleton = new Skeleton( [ root, child ] );
	const geometry = new BufferGeometry();

	geometry.setAttribute( 'position', new Float32BufferAttribute( [
		0, 0, 0, 1, 0, 0, 0, 1, 0, // triangle 0, bone 0
		1, 1, 0, 2, 1, 0, 1, 2, 0 // triangle 1, bone 1
	], 3 ) );
	geometry.setAttribute( 'skinIndex', new Uint16BufferAttribute( [
		0, 99, 99, 99,
		0, 99, 99, 99,
		0, 99, 99, 99,
		1, 99, 99, 99,
		1, 99, 99, 99,
		1, 99, 99, 99
	], 4 ) );
	geometry.setAttribute( 'skinWeight', new Float32BufferAttribute( [
		1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0,
		1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0
	], 4 ) );
	geometry.setIndex( [ 0, 1, 2, 3, 4, 5 ] );

	const attachedMesh = new SkinnedMesh( geometry, new MeshBasicMaterial() );
	attachedMesh.position.set( 3, 0, - 2 );
	attachedMesh.updateMatrixWorld( true );
	attachedMesh.bind( attachedSkeleton );

	const attachedPacks = partitionSkinnedMesh( attachedMesh, 1 );

	check( attachedPacks.length === 2, 'attached mesh split per bone' );
	compareDeformation( check, attachedMesh, attachedPacks, 'attached rest' );

	child.position.y = 3;
	root.updateMatrixWorld( true );
	attachedSkeleton.update();
	compareDeformation( check, attachedMesh, attachedPacks, 'attached animated' );

};

const isolation = ( check ) => {

	const { mesh, skeleton } = buildPackingMesh();

	const source = mesh.geometry;

	const digest = ( object ) => JSON.stringify( object );

	const snapshot = {
		position: Array.from( source.getAttribute( 'position' ).array ),
		skinIndex: Array.from( source.getAttribute( 'skinIndex' ).array ),
		skinWeight: Array.from( source.getAttribute( 'skinWeight' ).array ),
		index: Array.from( source.index.array ),
		groups: source.groups.map( group => ( { ...group } ) ),
		morphRelative: source.morphTargetsRelative,
		boneInverses: skeleton.boneInverses.map( matrix => Array.from( matrix.elements ) ),
		bindMatrix: Array.from( mesh.bindMatrix.elements ),
		bindMatrixInverse: Array.from( mesh.bindMatrixInverse.elements ),
		boneParents: skeleton.bones.map( bone => bone.parent ),
		boneChildren: skeleton.bones.map( bone => bone.children.slice() )
	};

	const packs = partitionSkinnedMesh( mesh, 2 );

	check( digest( Array.from( source.getAttribute( 'position' ).array ) ) === digest( snapshot.position ), 'source positions untouched' );
	check( digest( Array.from( source.getAttribute( 'skinIndex' ).array ) ) === digest( snapshot.skinIndex ), 'source skinIndex untouched' );
	check( digest( Array.from( source.getAttribute( 'skinWeight' ).array ) ) === digest( snapshot.skinWeight ), 'source skinWeight untouched' );
	check( digest( Array.from( source.index.array ) ) === digest( snapshot.index ), 'source index untouched' );
	check( digest( source.groups ) === digest( snapshot.groups ), 'source groups untouched' );
	check( source.morphTargetsRelative === snapshot.morphRelative, 'source morph flag untouched' );

	for ( let i = 0; i < skeleton.bones.length; i ++ ) {

		check( digest( Array.from( skeleton.boneInverses[ i ].elements ) ) === digest( snapshot.boneInverses[ i ] ), 'source inverse untouched' );
		check( skeleton.bones[ i ].parent === snapshot.boneParents[ i ], 'bone hierarchy parent untouched' );
		check(
			skeleton.bones[ i ].children.length === snapshot.boneChildren[ i ].length &&
			skeleton.bones[ i ].children.every( ( child, j ) => child === snapshot.boneChildren[ i ][ j ] ),
			'bone hierarchy children untouched'
		);

	}

	check( digest( Array.from( mesh.bindMatrix.elements ) ) === digest( snapshot.bindMatrix ), 'source bindMatrix untouched' );
	check( digest( Array.from( mesh.bindMatrixInverse.elements ) ) === digest( snapshot.bindMatrixInverse ), 'source bindMatrixInverse untouched' );

	// Every pack is independently complete: indexed, valid triangle coverage,
	// palette consistent with its remapped skin indices. Verified before any
	// output mutation below.
	for ( const pack of packs ) {

		check( pack.mesh.isSkinnedMesh === true, 'pack is SkinnedMesh' );
		check( pack.mesh.geometry.index !== null, 'pack geometry indexed' );

		const indexArray = Array.from( pack.mesh.geometry.index.array );
		check( indexArray.length === pack.sourceTriangleIndices.length * 3, 'index covers pack triangles' );

		const allowed = new Set( pack.boneIndices );
		const skinIndex = pack.mesh.geometry.getAttribute( 'skinIndex' );
		const skinWeight = pack.mesh.geometry.getAttribute( 'skinWeight' );

		for ( let vertex = 0; vertex < pack.sourceVertexIndices.length; vertex ++ ) {

			for ( let slot = 0; slot < 4; slot ++ ) {

				if ( skinWeight.getComponent( vertex, slot ) > 0 ) {

					check( skinIndex.getComponent( vertex, slot ) < pack.boneIndices.length, 'local bone number in range' );
					check( allowed.has( pack.boneIndices[ skinIndex.getComponent( vertex, slot ) ] ), 'local bone maps to palette entry' );

				}

			}

		}

	}

	// Writable output storage must not alias the source.

	const pack0 = packs[ 0 ];

	pack0.mesh.geometry.getAttribute( 'position' ).array[ 0 ] = 123.5;
	pack0.mesh.geometry.getAttribute( 'skinIndex' ).array[ 0 ] = 999;
	pack0.mesh.geometry.index.array[ 0 ] = 999;
	pack0.mesh.skeleton.boneInverses[ 0 ].elements[ 0 ] = 42;

	check( source.getAttribute( 'position' ).array[ 0 ] !== 123.5, 'position storage independent' );
	check( source.getAttribute( 'skinIndex' ).array[ 0 ] !== 999, 'skinIndex storage independent' );
	check( source.index.array[ 0 ] !== 999, 'index storage independent' );

	const sourceInverseHit = snapshot.boneInverses.some( inverse => inverse[ 0 ] === 42 );
	check( sourceInverseHit === false, 'inverse matrix storage independent' );

	// Morph influence arrays are copies.
	const sourceInfluences = mesh.morphTargetInfluences;

	if ( sourceInfluences !== undefined ) {

		for ( const pack of packs ) {

			check( pack.mesh.morphTargetInfluences !== sourceInfluences, 'morph influence array copied' );
			check( digest( pack.mesh.morphTargetInfluences ) === digest( sourceInfluences ), 'morph influence values match' );

		}

		pack0.mesh.morphTargetInfluences[ 0 ] = 0.9;
		check( sourceInfluences[ 0 ] !== 0.9, 'morph influence storage independent' );

	}

	// Local TRS copied from the source mesh.
	check( pack0.mesh.position.equals( mesh.position ), 'position copied' );
	check( pack0.mesh.quaternion.equals( mesh.quaternion ), 'quaternion copied' );
	check( pack0.mesh.scale.equals( mesh.scale ), 'scale copied' );
	check( pack0.mesh.matrix.equals( mesh.matrix ), 'local matrix copied' );
	check( pack0.mesh.matrixWorld.equals( mesh.matrixWorld ), 'world matrix copied' );

};

export const cases = { packing, attributes, deformation, isolation };
