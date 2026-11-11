import { TypeBackedArray } from '../TypeBackedArray.js';
import { Vector3, Vector4, Matrix3, BufferAttribute } from 'three';

const _vec3 = new Vector3();
const _vec3_0 = new Vector3();
const _vec3_1 = new Vector3();
const _vec3_2 = new Vector3();

const _vec4 = new Vector4();
const _vec4_0 = new Vector4();
const _vec4_1 = new Vector4();
const _vec4_2 = new Vector4();

const _norm = new Vector3();
const _tan = new Vector3();
const _mat3 = new Matrix3();

// Tangent frame convention
//
// The "tangent" attribute is a float4 render tangent frame: xyz is the tangent
// direction along U and w is exactly +1 or -1 giving the bitangent sign, so the
// bitangent is reconstructed as B = w * cross( N, T ). Every source triangle has
// the same w on all three of its vertices and mirrored UV charts are already
// split into separate vertices.
//
// The frame is carried through CSG with the following rules, applied equally to
// untouched vertices (a barycoord landing exactly on a source vertex) and to new
// vertices produced by triangle splitting:
//
// - the source normal and the source tangent xyz are interpolated in the source
//   geometry's local space using the barycentric coordinates within the source
//   triangle,
// - the normal is transformed by the inverse-transpose (normal matrix) and the
//   tangent xyz by the linear part of the relative transform - never by the
//   normal matrix and never by normalizing the whole vec4,
// - the tangent is then orthogonalized against the output normal and normalized,
// - w follows the source triangle and flips sign once per orientation reversal:
//   a reflection in the relative transform (negative determinant) is one
//   reversal, and a boolean operation inverting the source surface (eg the inner
//   walls produced by subtraction) is another, independent one. The tangent
//   direction along U is NOT flipped by surface inversion so the reconstructed
//   bitangent keeps pointing along the original texture direction.
//
// Degenerate input tangents, missing conventional attributes, and source
// triangles with mixed w signs are not supported.

function getBarycoordValue( a, b, c, barycoord, target, normalize = false, invert = false ) {

	target.set( 0, 0, 0, 0 )
		.addScaledVector( a, barycoord.x )
		.addScaledVector( b, barycoord.y )
		.addScaledVector( c, barycoord.z );

	if ( normalize ) {

		target.normalize();

	}

	if ( invert ) {

		target.multiplyScalar( - 1 );

	}

	return target;

}

function pushItemSize( vec, itemSize, target ) {

	switch ( itemSize ) {

		case 1:
			target.push( vec.x );
			break;

		case 2:
			target.push( vec.x, vec.y );
			break;

		case 3:
			target.push( vec.x, vec.y, vec.z );
			break;

		case 4:
			target.push( vec.x, vec.y, vec.z, vec.w );
			break;

	}

}

// orthogonalizes the tangent against the given output normal and normalizes it,
// falling back to an arbitrary perpendicular direction if the projection is
// degenerate so the result is always a finite unit vector
function orthonormalizeTangent( tangent, normal ) {

	tangent.addScaledVector( normal, - tangent.dot( normal ) );
	if ( ! ( tangent.lengthSq() > 1e-20 ) ) {

		tangent.set( 1, 0, 0 );
		if ( Math.abs( normal.x ) > 0.9 ) {

			tangent.set( 0, 1, 0 );

		}

		tangent.addScaledVector( normal, - tangent.dot( normal ) );

	}

	return tangent.normalize();

}

class AttributeData extends TypeBackedArray {

	get count() {

		return this.length / this.itemSize;

	}

	constructor( ...args ) {

		super( ...args );
		this.itemSize = 1;
		this.normalized = false;


	}

}

export class GeometryBuilder {

	constructor() {

		this.attributeData = {};
		this.groupIndices = [];
		this.forwardIndexMap = new Map();
		this.invertedIndexMap = new Map();
		this.interpolatedFields = {};

		// linear part of the relative transform used to carry tangent directions
		// into the result frame, cached by initInterpolatedAttributeData
		this._linearMatrix = new Matrix3();

	}

	initFromGeometry( referenceGeometry, relevantAttributes ) {

		this.clear();

		// initialize and clear unused data from the attribute buffers and vice versa
		const { attributeData } = this;
		const refAttributes = referenceGeometry.attributes;
		for ( let i = 0, l = relevantAttributes.length; i < l; i ++ ) {

			const key = relevantAttributes[ i ];
			const refAttr = refAttributes[ key ];
			const type = refAttr.array.constructor;
			if ( ! attributeData[ key ] ) {

				attributeData[ key ] = new AttributeData( type );

			}

			attributeData[ key ].setType( type );
			attributeData[ key ].itemSize = refAttr.itemSize;
			attributeData[ key ].normalized = refAttr.normalized;

		}

		for ( const key in attributeData ) {

			if ( ! relevantAttributes.includes( key ) ) {

				delete attributeData[ key ];

			}

		}

	}

	// init and cache all the attribute data for the given indices so we can use it to append interpolated attribute data
	initInterpolatedAttributeData( geometry, matrix, normalMatrix, i0, i1, i2 ) {

		const { attributeData, interpolatedFields } = this;
		const { attributes } = geometry;

		this._linearMatrix.setFromMatrix4( matrix );

		for ( const key in attributeData ) {

			const attr = attributes[ key ];
			if ( ! attr ) {

				throw new Error( `CSG Operations: Attribute ${ key } not available on geometry.` );

			}

			// handle normals and positions specially because they require transforming
			let v0, v1, v2;
			if ( key === 'position' ) {

				v0 = _vec3_0.fromBufferAttribute( attr, i0 ).applyMatrix4( matrix );
				v1 = _vec3_1.fromBufferAttribute( attr, i1 ).applyMatrix4( matrix );
				v2 = _vec3_2.fromBufferAttribute( attr, i2 ).applyMatrix4( matrix );

			} else if ( key === 'normal' ) {

				v0 = _vec3_0.fromBufferAttribute( attr, i0 ).applyNormalMatrix( normalMatrix );
				v1 = _vec3_1.fromBufferAttribute( attr, i1 ).applyNormalMatrix( normalMatrix );
				v2 = _vec3_2.fromBufferAttribute( attr, i2 ).applyNormalMatrix( normalMatrix );

			} else if ( key === 'tangent' ) {

				// cache the source-space tangent frame (xyz along U, w = ±1 bitangent
				// sign). The transform into the result frame happens after the
				// barycentric interpolation - see appendInterpolatedAttributeData.
				v0 = _vec4_0.fromBufferAttribute( attr, i0 );
				v1 = _vec4_1.fromBufferAttribute( attr, i1 );
				v2 = _vec4_2.fromBufferAttribute( attr, i2 );

			} else {

				v0 = _vec4_0.fromBufferAttribute( attr, i0 );
				v1 = _vec4_1.fromBufferAttribute( attr, i1 );
				v2 = _vec4_2.fromBufferAttribute( attr, i2 );

			}

			if ( ! interpolatedFields[ key ] ) {

				interpolatedFields[ key ] = [ v0.clone(), v1.clone(), v2.clone() ];

			} else {

				const fields = interpolatedFields[ key ];
				fields[ 0 ].copy( v0 );
				fields[ 1 ].copy( v1 );
				fields[ 2 ].copy( v2 );

			}

		}

	}

	// push data from the given barycoord onto the geometry
	appendInterpolatedAttributeData( group, barycoord, index = null, invert = false ) {

		const { groupIndices, attributeData, interpolatedFields, forwardIndexMap, invertedIndexMap } = this;
		while ( groupIndices.length <= group ) {

			groupIndices.push( new AttributeData( Uint32Array ) );

		}

		const indexMap = invert ? invertedIndexMap : forwardIndexMap;
		const indexData = groupIndices[ group ];
		if ( index !== null && indexMap.has( index ) ) {

			indexData.push( indexMap.get( index ) );

		} else {

			indexMap.set( index, attributeData.position.count );
			indexData.push( attributeData.position.count );

			// if a tangent frame is being generated then resolve the output normal
			// first so the tangent can be orthogonalized against it
			let tangentNormal = null;
			if ( 'tangent' in interpolatedFields && 'normal' in interpolatedFields ) {

				const [ n0, n1, n2 ] = interpolatedFields.normal;
				tangentNormal = getBarycoordValue( n0, n1, n2, barycoord, _norm, true, invert );

			}

			for ( const key in interpolatedFields ) {

				const arr = attributeData[ key ];
				if ( key === 'tangent' ) {

					// interpolate the tangent direction in the source frame, transform
					// it by the linear part of the relative transform and
					// re-orthogonalize it against the output normal. The w component
					// follows the source triangle and flips with the triangle
					// inversion (boolean surface inversion XOR transform reflection)
					// while the direction along U does not.
					const [ t0, t1, t2 ] = interpolatedFields.tangent;
					_tan.set( 0, 0, 0 )
						.addScaledVector( t0, barycoord.x )
						.addScaledVector( t1, barycoord.y )
						.addScaledVector( t2, barycoord.z )
						.applyMatrix3( this._linearMatrix );

					let w = t0.w < 0 ? - 1 : 1;
					if ( invert ) {

						w = - w;

					}

					if ( tangentNormal ) {

						orthonormalizeTangent( _tan, tangentNormal );

					} else {

						_tan.normalize();

					}

					arr.push( _tan.x, _tan.y, _tan.z, w );

				} else {

					// handle normals and positions specially because they require transforming
					const isDirection = key === 'normal';
					const invertVector = invert && isDirection;
					const itemSize = arr.itemSize;
					const [ v0, v1, v2 ] = interpolatedFields[ key ];
					getBarycoordValue( v0, v1, v2, barycoord, _vec4, isDirection, invertVector );
					pushItemSize( _vec4, itemSize, arr );

				}

			}

		}

	}

	// append the given vertex index from the source geometry to this one
	appendIndexFromGeometry( geometry, matrix, normalMatrix, group, index, invert = false ) {

		const { groupIndices, attributeData, forwardIndexMap, invertedIndexMap } = this;
		while ( groupIndices.length <= group ) {

			groupIndices.push( new AttributeData( Uint32Array ) );

		}

		const indexMap = invert ? invertedIndexMap : forwardIndexMap;
		const indexData = groupIndices[ group ];
		if ( index !== null && indexMap.has( index ) ) {

			indexData.push( indexMap.get( index ) );

		} else {

			indexMap.set( index, attributeData.position.count );
			indexData.push( attributeData.position.count );

			const { attributes } = geometry;
			for ( const key in attributeData ) {

				const arr = attributeData[ key ];
				const attr = attributes[ key ];
				if ( ! attr ) {

					throw new Error( `CSG Operations: Attribute ${ key } not available on geometry.` );

				}

				// specially handle the position and normal attributes because they require transforms
				const itemSize = attr.itemSize;
				if ( key === 'position' ) {

					_vec3.fromBufferAttribute( attr, index ).applyMatrix4( matrix );
					arr.push( _vec3.x, _vec3.y, _vec3.z );

				} else if ( key === 'normal' ) {

					_vec3.fromBufferAttribute( attr, index ).applyNormalMatrix( normalMatrix );
					if ( invert ) {

						_vec3.multiplyScalar( - 1 );

					}

					arr.push( _vec3.x, _vec3.y, _vec3.z );

				} else if ( key === 'tangent' ) {

					// transform the tangent direction by the linear part of the
					// relative transform and re-orthogonalize it against the output
					// normal. The w component follows the source triangle and flips
					// with the triangle inversion (boolean surface inversion XOR
					// transform reflection) while the direction along U does not.
					_vec4.fromBufferAttribute( attr, index );
					_tan.set( _vec4.x, _vec4.y, _vec4.z )
						.applyMatrix3( _mat3.setFromMatrix4( matrix ) );

					let w = _vec4.w < 0 ? - 1 : 1;
					if ( invert ) {

						w = - w;

					}

					const normalAttr = attributes.normal;
					if ( normalAttr ) {

						_norm.fromBufferAttribute( normalAttr, index ).applyNormalMatrix( normalMatrix );
						if ( invert ) {

							_norm.multiplyScalar( - 1 );

						}

						orthonormalizeTangent( _tan, _norm );

					} else {

						_tan.normalize();

					}

					arr.push( _tan.x, _tan.y, _tan.z, w );

				} else {

					_vec4.fromBufferAttribute( attr, index );
					pushItemSize( _vec4, itemSize, arr );

				}

			}

		}

	}

	buildGeometry( target, groupOrder ) {

		let needsDisposal = false;
		const { groupIndices, attributeData } = this;
		const { attributes, index } = target;
		for ( const key in attributeData ) {

			const arr = attributeData[ key ];
			const { type, itemSize, normalized, length, count } = arr;
			const buffer = arr.array.buffer;

			let attr = attributes[ key ];
			if ( ! attr || attr.count < count || attr.array.type !== type ) {

				// create the attribute if it doesn't exist yet
				attr = new BufferAttribute( new type( length ), itemSize, normalized );
				target.setAttribute( key, attr );
				needsDisposal = true;

			}

			// copy the data
			attr.array.set( new type( buffer, 0, length ), 0 );
			attr.needsUpdate = true;

		}

		// remove or update the index appropriately
		const indexCount = groupIndices.reduce( ( v, arr ) => arr.count + v, 0 );
		if ( ! target.index || index.count < indexCount || index.array.type !== Uint32Array ) {

			target.setIndex( new BufferAttribute( new Uint32Array( indexCount ), 1 ) );
			needsDisposal = true;

		}

		// initialize the groups
		target.clearGroups();

		let offset = 0;
		for ( let i = 0, l = Math.min( groupOrder.length, groupIndices.length ); i < l; i ++ ) {

			const { index, materialIndex } = groupOrder[ i ];
			const { count } = groupIndices[ index ];
			const buffer = groupIndices[ index ].array.buffer;
			if ( count !== 0 ) {

				target.index.array.set( new Uint32Array( buffer, 0, count ), offset );
				target.addGroup( offset, count, materialIndex );
				offset += count;

			}

		}

		// update the draw range
		target.setDrawRange( 0, offset );

		// remove the bounds tree if it exists because its now out of date
		// TODO: can we have this dispose in the same way that a brush does?
		// TODO: why are half edges and group indices not removed here?
		target.boundsTree = null;
		target.boundingBox = null;
		target.boundingSphere = null;

		if ( needsDisposal ) {

			target.dispose();

		}

	}

	clearIndexMap() {

		this.forwardIndexMap.clear();
		this.invertedIndexMap.clear();

	}

	clear() {

		const { groupIndices, attributeData } = this;

		this.interpolatedFields = {};

		for ( const key in attributeData ) {

			attributeData[ key ].clear();

		}

		groupIndices.forEach( arr => {

			arr.clear();

		} );
		this.clearIndexMap();

	}

}
