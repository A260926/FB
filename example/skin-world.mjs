/**
 * Example: CPU side skinned world positions, culling and picking.
 *
 * ClayGL deforms skinned meshes on the GPU. Two Mesh methods expose the
 * same skinned space on the CPU, in world coordinates:
 *
 *   mesh.getSkinnedWorldPositions()   -> Float32Array, xyz per vertex
 *   mesh.getSkinnedWorldBoundingBox() -> clay.BoundingBox around them
 *
 * Both follow the shader skinning rule: joint matrices are
 * `jointWorldTransform * inverseBindMatrix` of the joints referenced by
 * `mesh.joints` (the local palette into `mesh.skeleton.joints`), the first
 * weight is always added, the other three only when larger than 1e-4 and
 * the fourth weight is `1 - weight.x - weight.y - weight.z`. The weighted
 * homogeneous positions are transformed by `mesh.offsetMatrix` (identity
 * when unset) and divided by the homogeneous denominator at the end.
 *
 * Scene#updateRenderList culls every skinned instance with its own
 * deformed bounding box and RayPicking intersects the deformed triangles
 * of each instance, so meshes that share one skeleton (and use
 * offsetMatrix) are culled and picked where they are actually drawn.
 *
 * Run it (Node 26, no browser/GPU needed):
 *
 *   node --permission --allow-fs-read='*' --no-addons --no-warnings \
 *        --import ./task_support/bootstrap.mjs example/skin-world.mjs
 */
import Scene from '../src/Scene.js';
import Node from '../src/Node.js';
import Joint from '../src/Joint.js';
import Skeleton from '../src/Skeleton.js';
import Matrix4 from '../src/math/Matrix4.js';
import {triangle, mesh, cameraAt, picker} from '../task_support/fixtures.mjs';

// 1. A skeleton of two joints, bound at the origin.
var skeleton = new Skeleton();
skeleton.joints = [0, 1].map(function (index) {
    return new Joint({index: index, node: new Node()});
});
for (var i = 0; i < skeleton.joints.length; i++) {
    skeleton.joints[i].node.update();
}
skeleton.updateJointMatrices();

// 2. Pose the second joint ten units to the right and update the skeleton,
//    exactly like the animation loop would every frame.
skeleton.joints[1].node.position.set(10, 0, 0);
skeleton.joints[1].node.update();
skeleton.update();

// 3. A triangle fully weighted (through the fourth weight) to local joint 1.
var geometry = triangle([0, 0, 0], [0, 0, 0, 1]);

// 4. Two skinned meshes share skeleton and geometry; offsetMatrix places
//    the clones 20 units left and right of the deformed pose.
var left = mesh(geometry);
var right = mesh(geometry);
for (var k = 0; k < 2; k++) {
    var m = [left, right][k];
    m.skeleton = skeleton;
    m.joints = [0, 1];
    m.offsetMatrix = new Matrix4();
}
left.offsetMatrix.array[12] = -20;
right.offsetMatrix.array[12] = 20;

var scene = new Scene();
scene.add(left);
scene.add(right);
scene.update();

// 5. World space positions of each instance, recomputed on every call.
console.log('left  positions:', Array.from(left.getSkinnedWorldPositions()));
// -> [-10, 0, 0, -9, 0, 0, -10, 1, 0]
console.log('right positions:', Array.from(right.getSkinnedWorldPositions()));
// -> [30, 0, 0, 31, 0, 0, 30, 1, 0]
var box = right.getSkinnedWorldBoundingBox();
console.log('right box:', box.min.array, box.max.array);
// -> [30, 0, 0] [31, 1, 0]

// 6. Frustum culling uses the deformed box of each instance: looking at the
//    right clone only lists the right clone.
var camera = cameraAt(30.25);
var list = scene.updateRenderList(camera);
console.log('visible:', list.opaque.map(function (m) { return m === right ? 'right' : 'left'; }));
// -> ['right']

// 7. Picking hits the deformed triangles: `pointWorld` is the world hit,
//    `point` the deformed hit before offsetMatrix, `distance` is measured
//    from the ray origin.
var hits = picker(scene, camera).pickAll(50, 50);
console.log('hit target is right:', hits[0].target === right);
console.log('hit pointWorld:', hits[0].pointWorld.array);
console.log('hit point (before offset):', hits[0].point.array);
console.log('hit distance:', hits[0].distance);
