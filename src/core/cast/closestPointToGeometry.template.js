import { Vector3, Matrix4, Box3 } from 'three';
import { OrientedBox } from '../../math/OrientedBox.js';
import { setTriangle } from '../../utils/TriangleUtilities.js';
import { getTriCount } from '../build/geometryUtils.js';
import { ExtendedTrianglePool } from '../../utils/ExtendedTrianglePool.js';

const tempMatrix = /* @__PURE__ */ new Matrix4();
const obb = /* @__PURE__ */ new OrientedBox();
const leafBox = /* @__PURE__ */ new Box3();
const otherBox = /* @__PURE__ */ new Box3();
const temp1 = /* @__PURE__ */ new Vector3();
const temp2 = /* @__PURE__ */ new Vector3();
const temp3 = /* @__PURE__ */ new Vector3();
const temp4 = /* @__PURE__ */ new Vector3();

const boxFields = [ 'x', 'y', 'z' ];

// Returns the distance between two axis aligned boxes. The boxes have to be expressed in the same
// frame so the result is an actual distance rather than a per-axis gap measured in different
// units. All distance and threshold comparisons in this function run in the BVH local frame.
function distanceToBox( box1, box2 ) {

	let distSq = 0;
	for ( let i = 0; i < 3; i ++ ) {

		const field = boxFields[ i ];
		const d = Math.max(
			0,
			box1.min[ field ] - box2.max[ field ],
			box2.min[ field ] - box1.max[ field ],
		);
		distSq += d * d;

	}

	return Math.sqrt( distSq );

}

export function closestPointToGeometry/* @echo INDIRECT_STRING */(
	bvh,
	otherGeometry,
	geometryToBvh,
	target1 = { },
	target2 = { },
	minThreshold = 0,
	maxThreshold = Infinity,
) {

	if ( ! otherGeometry.boundingBox ) {

		otherGeometry.computeBoundingBox();

	}

	// oriented box representing the full other geometry in the BVH frame. Even with non-uniform
	// scaling the transformed axis-aligned box stays a valid (conservative) bounds of the geometry.
	obb.set( otherGeometry.boundingBox.min, otherGeometry.boundingBox.max, geometryToBvh );
	obb.needsUpdate = true;

	const geometry = bvh.geometry;
	const pos = geometry.attributes.position;
	const index = geometry.index;
	const otherPos = otherGeometry.attributes.position;
	const otherIndex = otherGeometry.index;
	const triangle = ExtendedTrianglePool.getPrimitive();
	const triangle2 = ExtendedTrianglePool.getPrimitive();

	let tempTarget1 = temp1;
	let tempTargetDest1 = temp2;
	let tempTarget2 = null;
	let tempTargetDest2 = null;

	if ( target2 ) {

		tempTarget2 = temp3;
		tempTargetDest2 = temp4;

	}

	let closestDistance = Infinity;
	let closestDistanceTriIndex = null;
	let closestDistanceOtherTriIndex = null;
	tempMatrix.copy( geometryToBvh ).invert();
	bvh.shapecast(
		{

			boundsTraverseOrder: box => {

				return obb.distanceToBox( box );

			},

			intersectsBounds: ( box, isLeaf, score ) => {

				return score < closestDistance && score < maxThreshold;

			},

			intersectsRange: ( offset, count, contained, depth, nodeIndex, box ) => {

				// the bounds are passed directly to the range function which also covers the case
				// where the root node is a leaf (and "intersectsBounds" is never invoked for it)
				leafBox.copy( box );

				if ( otherGeometry.boundsTree ) {

					// if the other geometry has a bvh then use the accelerated path where we use shapecast to find
					// the closest bounds in the other geometry to check.
					const otherBvh = otherGeometry.boundsTree;
					return otherBvh.shapecast( {
						boundsTraverseOrder: box => {

							// transform the other bounds into the BVH frame so the gap is measured in the
							// same units as the closest distance and the thresholds. Comparing boxes in the
							// other geometry frame would mix units whenever the transform contains a
							// non-uniform or negative scale.
							otherBox.copy( box ).applyMatrix4( geometryToBvh );
							return distanceToBox( leafBox, otherBox );

						},

						intersectsBounds: ( box, isLeaf, score ) => {

							return score < closestDistance && score < maxThreshold;

						},

						intersectsRange: ( otherOffset, otherCount ) => {

							for ( let i2 = otherOffset, l2 = otherOffset + otherCount; i2 < l2; i2 ++ ) {

								// the other BVH may be indirect even if this one is not so the index has
								// to be resolved at runtime instead of at preprocess time
								const ti2 = otherBvh.indirect ? otherBvh.resolveTriangleIndex( i2 ) : i2;
								setTriangle( triangle2, 3 * ti2, otherIndex, otherPos );

								triangle2.a.applyMatrix4( geometryToBvh );
								triangle2.b.applyMatrix4( geometryToBvh );
								triangle2.c.applyMatrix4( geometryToBvh );
								triangle2.needsUpdate = true;

								for ( let i = offset, l = offset + count; i < l; i ++ ) {

									/* @if INDIRECT */

									const ti = bvh.resolveTriangleIndex( i );
									setTriangle( triangle, 3 * ti, index, pos );

									/* @else */

									const ti = i;
									setTriangle( triangle, 3 * i, index, pos );

									/* @endif */
									triangle.needsUpdate = true;

									const dist = triangle.distanceToTriangle( triangle2, tempTarget1, tempTarget2 );
									if ( dist < closestDistance && dist < maxThreshold ) {

										tempTargetDest1.copy( tempTarget1 );

										if ( tempTargetDest2 ) {

											tempTargetDest2.copy( tempTarget2 );

										}

										closestDistance = dist;
										// store the triangle index in the geometry's own index buffer so it can be
										// passed straight to getTriangleHitPointInfo regardless of BVH layout
										closestDistanceTriIndex = ti;
										closestDistanceOtherTriIndex = ti2;

									}

									// stop traversal if we find a point that's under the given threshold
									if ( dist < minThreshold ) {

										return true;

									}

								}

							}

						},
					} );

				} else {

					// If no bounds tree then we'll just check every triangle.
					const triCount = getTriCount( otherGeometry );
					for ( let i2 = 0, l2 = triCount; i2 < l2; i2 ++ ) {

						setTriangle( triangle2, 3 * i2, otherIndex, otherPos );
						triangle2.a.applyMatrix4( geometryToBvh );
						triangle2.b.applyMatrix4( geometryToBvh );
						triangle2.c.applyMatrix4( geometryToBvh );
						triangle2.needsUpdate = true;

						for ( let i = offset, l = offset + count; i < l; i ++ ) {

							/* @if INDIRECT */

							const ti = bvh.resolveTriangleIndex( i );
							setTriangle( triangle, 3 * ti, index, pos );

							/* @else */

							const ti = i;
							setTriangle( triangle, 3 * i, index, pos );

							/* @endif */
							triangle.needsUpdate = true;

							const dist = triangle.distanceToTriangle( triangle2, tempTarget1, tempTarget2 );
							if ( dist < closestDistance && dist < maxThreshold ) {

								tempTargetDest1.copy( tempTarget1 );

								if ( tempTargetDest2 ) {

									tempTargetDest2.copy( tempTarget2 );

								}

								closestDistance = dist;
								// store the triangle index in the geometry's own index buffer so it can be
								// passed straight to getTriangleHitPointInfo regardless of BVH layout
								closestDistanceTriIndex = ti;
								closestDistanceOtherTriIndex = i2;

							}

							// stop traversal if we find a point that's under the given threshold
							if ( dist < minThreshold ) {

								return true;

							}

						}

					}

				}

			},

		}

	);

	ExtendedTrianglePool.releasePrimitive( triangle );
	ExtendedTrianglePool.releasePrimitive( triangle2 );

	if ( closestDistance === Infinity ) {

		return null;

	}

	if ( ! target1.point ) {

		target1.point = tempTargetDest1.clone();

	} else {

		target1.point.copy( tempTargetDest1 );

	}

	target1.distance = closestDistance,
	target1.faceIndex = closestDistanceTriIndex;

	if ( target2 ) {

		if ( ! target2.point ) target2.point = tempTargetDest2.clone();
		else target2.point.copy( tempTargetDest2 );
		target2.point.applyMatrix4( tempMatrix );
		tempTargetDest1.applyMatrix4( tempMatrix );
		target2.distance = tempTargetDest1.sub( target2.point ).length();
		target2.faceIndex = closestDistanceOtherTriIndex;

	}

	return target1;

}
