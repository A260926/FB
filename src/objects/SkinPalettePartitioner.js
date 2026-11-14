import { BufferAttribute } from '../core/BufferAttribute.js';
import { BufferGeometry } from '../core/BufferGeometry.js';
import { SkinnedMesh } from './SkinnedMesh.js';
import { Skeleton } from './Skeleton.js';

/**
 * A single independently drawable result of {@link partitionSkinnedMesh}.
 *
 * @typedef {Object} SkinPack
 * @property {SkinnedMesh} mesh - The drawable skinned mesh for this pack.
 * @property {Array<number>} boneIndices - Palette bones mapped back to input bone numbers, ascending.
 * @property {Array<number>} sourceVertexIndices - Pack vertices mapped back to input vertex numbers.
 * @property {Array<number>} sourceTriangleIndices - Pack triangles mapped back to input triangle numbers.
 */

/**
 * Splits a SkinnedMesh into independently drawable packs whose bone palettes
 * fit into a fixed number of matrix slots.
 *
 * Packs are built greedily over the input triangles in order. The bones
 * referenced by any positive skin-weight slot of a triangle's three vertices
 * share the palette (each bone counts once; zero-weight slots do not count).
 *
 * Each returned pack is a fully usable SkinnedMesh backed by an indexed
 * BufferGeometry whose vertices are remapped into pack-local numbering.
 * Bones and materials keep their source object identity; every writable
 * buffer, inverse bind matrix and morph weight array is copied.
 *
 * @param {SkinnedMesh} mesh - The skinned mesh to partition.
 * @param {number} maxBones - Maximum number of bones per pack (positive integer).
 * @return {Array<SkinPack>} The resulting packs.
 */
function partitionSkinnedMesh( mesh, maxBones ) {

	if ( Number.isInteger( maxBones ) !== true || maxBones <= 0 ) {

		throw new TypeError( 'partitionSkinnedMesh: maxBones must be a positive integer.' );

	}

	const sourceGeometry = mesh.geometry;
	const sourceSkeleton = mesh.skeleton;
	const sourceSkinIndex = sourceGeometry.getAttribute( 'skinIndex' );
	const sourceSkinWeight = sourceGeometry.getAttribute( 'skinWeight' );

	const index = sourceGeometry.getIndex();
	const vertexCount = sourceGeometry.getAttribute( 'position' ).count;
	const triangleCount = index !== null ? index.count / 3 : vertexCount / 3;

	// Resolve each input triangle to its three source vertex indices.

	const triangleVertices = new Array( triangleCount );

	for ( let t = 0; t < triangleCount; t ++ ) {

		if ( index !== null ) {

			const i = t * 3;
			triangleVertices[ t ] = [ index.getX( i ), index.getX( i + 1 ), index.getX( i + 2 ) ];

		} else {

			const i = t * 3;
			triangleVertices[ t ] = [ i, i + 1, i + 2 ];

		}

	}

	// Material index of each source triangle, derived from the geometry groups.

	const triangleMaterial = new Array( triangleCount );

	if ( sourceGeometry.groups.length > 0 ) {

		for ( const group of sourceGeometry.groups ) {

			for ( let i = group.start; i < group.start + group.count; i += 3 ) {

				triangleMaterial[ i / 3 ] = group.materialIndex;

			}

		}

	}

	const weightStride = sourceSkinWeight.isInterleavedBufferAttribute === true ? sourceSkinWeight.data.stride : sourceSkinWeight.itemSize;
	const weightOffset = sourceSkinWeight.isInterleavedBufferAttribute === true ? sourceSkinWeight.offset : 0;
	const weightArray = sourceSkinWeight.array;

	const indexStride = sourceSkinIndex.isInterleavedBufferAttribute === true ? sourceSkinIndex.data.stride : sourceSkinIndex.itemSize;
	const indexOffset = sourceSkinIndex.isInterleavedBufferAttribute === true ? sourceSkinIndex.offset : 0;
	const skinIndexArray = sourceSkinIndex.array;
	const skinSlotCount = sourceSkinIndex.itemSize;

	function getWeight( vertex, slot ) {

		return weightArray[ vertex * weightStride + weightOffset + slot ];

	}

	function getSkinIndex( vertex, slot ) {

		return skinIndexArray[ vertex * indexStride + indexOffset + slot ];

	}

	// Collect the distinct positively weighted bones of each triangle.

	const triangleBones = new Array( triangleCount );

	for ( let t = 0; t < triangleCount; t ++ ) {

		const bones = new Set();

		for ( const vertex of triangleVertices[ t ] ) {

			for ( let slot = 0; slot < skinSlotCount; slot ++ ) {

				if ( getWeight( vertex, slot ) !== 0 ) {

					bones.add( getSkinIndex( vertex, slot ) );

				}

			}

		}

		if ( bones.size > maxBones ) {

			throw new RangeError( `partitionSkinnedMesh: triangle ${ t } references ${ bones.size } bones, which exceeds maxBones ${ maxBones }.` );

		}

		triangleBones[ t ] = bones;

	}

	// Greedy contiguous packing over the input triangle order.

	const packBuilders = [];
	let current = null;

	function startPack() {

		return {
			triangles: [],
			boneSet: new Set(),
			vertices: [],
			vertexMap: new Map()
		};

	}

	for ( let t = 0; t < triangleCount; t ++ ) {

		const bones = triangleBones[ t ];

		if ( current === null ) current = startPack();

		let additional = 0;

		for ( const bone of bones ) {

			if ( current.boneSet.has( bone ) === false ) additional ++;

		}

		if ( current.boneSet.size + additional > maxBones ) {

			packBuilders.push( current );
			current = startPack();

		}

		current.triangles.push( t );

		for ( const bone of bones ) current.boneSet.add( bone );

		for ( const vertex of triangleVertices[ t ] ) {

			if ( current.vertexMap.has( vertex ) === false ) {

				current.vertexMap.set( vertex, current.vertices.length );
				current.vertices.push( vertex );

			}

		}

	}

	if ( current !== null ) packBuilders.push( current );

	// Copies one vertex item out of a regular or interleaved attribute.

	function readItem( attribute, vertex, targetArray, targetOffset ) {

		const sourceArray = attribute.array;
		const itemSize = attribute.itemSize;

		if ( attribute.isInterleavedBufferAttribute === true ) {

			const sourceOffset = vertex * attribute.data.stride + attribute.offset;

			for ( let k = 0; k < itemSize; k ++ ) targetArray[ targetOffset + k ] = sourceArray[ sourceOffset + k ];

		} else {

			const sourceOffset = vertex * itemSize;

			for ( let k = 0; k < itemSize; k ++ ) targetArray[ targetOffset + k ] = sourceArray[ sourceOffset + k ];

		}

	}

	function remapAttribute( attribute, sourceVertices ) {

		const itemSize = attribute.itemSize;

		// De-interleave into a plain BufferAttribute when necessary; otherwise
		// reuse the source's concrete attribute class so typed storage (e.g.
		// half-float) and itemSize/normalized semantics are preserved.
		const AttributeClass = attribute.isInterleavedBufferAttribute === true
			? BufferAttribute
			: attribute.constructor;

		const result = new AttributeClass(
			new attribute.array.constructor( sourceVertices.length * itemSize ),
			itemSize,
			attribute.normalized
		);

		if ( attribute.name !== '' ) result.name = attribute.name;

		for ( let local = 0; local < sourceVertices.length; local ++ ) {

			readItem( attribute, sourceVertices[ local ], result.array, local * itemSize );

		}

		return result;

	}

	const packs = [];

	for ( const builder of packBuilders ) {

		const sourceVertices = builder.vertices;
		const boneIndices = Array.from( builder.boneSet ).sort( ( a, b ) => a - b );
		const localBoneIndex = new Map();

		for ( let local = 0; local < boneIndices.length; local ++ ) {

			localBoneIndex.set( boneIndices[ local ], local );

		}

		const geometry = new BufferGeometry();

		// Vertex attributes, de-interleaved where necessary but with unchanged
		// typed-array type, itemSize and normalized semantics.

		for ( const name in sourceGeometry.attributes ) {

			const attribute = sourceGeometry.attributes[ name ];
			const remapped = remapAttribute( attribute, sourceVertices );

			if ( name === 'skinIndex' ) {

				for ( let local = 0; local < sourceVertices.length; local ++ ) {

					const sourceVertex = sourceVertices[ local ];

					for ( let slot = 0; slot < skinSlotCount; slot ++ ) {

						const offset = local * skinSlotCount + slot;

						if ( getWeight( sourceVertex, slot ) !== 0 ) {

							remapped.array[ offset ] = localBoneIndex.get( getSkinIndex( sourceVertex, slot ) );

						} else {

							remapped.array[ offset ] = 0;

						}

					}

				}

			}

			geometry.setAttribute( name, remapped );

		}

		// Morph target channels follow the same vertex remapping.

		for ( const name in sourceGeometry.morphAttributes ) {

			geometry.morphAttributes[ name ] = sourceGeometry.morphAttributes[ name ].map( attribute => remapAttribute( attribute, sourceVertices ) );

		}

		geometry.morphTargetsRelative = sourceGeometry.morphTargetsRelative;

		// Pack-local index buffer.

		const localIndices = [];

		for ( const t of builder.triangles ) {

			for ( const vertex of triangleVertices[ t ] ) {

				localIndices.push( builder.vertexMap.get( vertex ) );

			}

		}

		geometry.setIndex( localIndices );

		// Groups keep the material index of every source triangle, merged into
		// contiguous runs of identical material indices.

		if ( sourceGeometry.groups.length > 0 ) {

			let groupStart = 0;
			let groupMaterialIndex = triangleMaterial[ builder.triangles[ 0 ] ];

			for ( let i = 1; i <= builder.triangles.length; i ++ ) {

				const materialIndex = i < builder.triangles.length ? triangleMaterial[ builder.triangles[ i ] ] : - 1;

				if ( materialIndex !== groupMaterialIndex ) {

					geometry.addGroup( groupStart, i * 3 - groupStart, groupMaterialIndex );
					groupStart = i * 3;
					groupMaterialIndex = materialIndex;

				}

			}

		}

		const partMesh = new SkinnedMesh( geometry, mesh.material );

		partMesh.position.copy( mesh.position );
		partMesh.quaternion.copy( mesh.quaternion );
		partMesh.scale.copy( mesh.scale );
		partMesh.matrix.copy( mesh.matrix );
		partMesh.matrixWorld.copy( mesh.matrixWorld );
		partMesh.matrixAutoUpdate = mesh.matrixAutoUpdate;
		partMesh.matrixWorldAutoUpdate = mesh.matrixWorldAutoUpdate;

		const partBones = boneIndices.map( boneIndex => sourceSkeleton.bones[ boneIndex ] );
		const partBoneInverses = boneIndices.map( boneIndex => sourceSkeleton.boneInverses[ boneIndex ].clone() );
		const partSkeleton = new Skeleton( partBones, partBoneInverses );

		partMesh.bind( partSkeleton, mesh.bindMatrix );
		partMesh.bindMode = mesh.bindMode;
		partMesh.bindMatrix.copy( mesh.bindMatrix );
		partMesh.bindMatrixInverse.copy( mesh.bindMatrixInverse );

		if ( mesh.morphTargetInfluences !== undefined ) {

			partMesh.morphTargetInfluences = mesh.morphTargetInfluences.slice();

		}

		packs.push( {
			mesh: partMesh,
			boneIndices: boneIndices,
			sourceVertexIndices: sourceVertices.slice(),
			sourceTriangleIndices: builder.triangles.slice()
		} );

	}

	return packs;

}

export { partitionSkinnedMesh };
