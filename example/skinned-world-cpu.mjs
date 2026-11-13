// Runnable CPU skinning example (no browser / GPU needed).
//
// Run with Node 26 and the repository's extension-resolving loader:
//
//   node --permission --allow-fs-read='*' --no-addons --no-warnings \
//     --import ./task_support/bootstrap.mjs example/skinned-world-cpu.mjs
//
// It builds two skinned meshes that share one skeleton and one geometry, gives
// each instance its own offsetMatrix, then reads the per-instance world
// positions, bounding box and ray picking results.
import Geometry from '../src/Geometry';
import Scene from '../src/Scene';
import Mesh from '../src/Mesh';
import Node from '../src/Node';
import Joint from '../src/Joint';
import Skeleton from '../src/Skeleton';
import Perspective from '../src/camera/Perspective';
import Vector3 from '../src/math/Vector3';
import Matrix4 from '../src/math/Matrix4';
import RayPicking from '../src/picking/RayPicking';
import Renderer from '../src/Renderer';

// One indexed triangle, fully bound to joint index 1 of each mesh.
var geometry = new Geometry();
geometry.attributes.position.fromArray([[0, 0, 0], [1, 0, 0], [0, 1, 0]]);
geometry.attributes.weight.fromArray([[1, 0, 0], [1, 0, 0], [1, 0, 0]]);
geometry.attributes.joint.fromArray([[1, 0, 0, 1], [1, 0, 0, 1], [1, 0, 0, 1]]);
geometry.initIndicesFromArray([[0, 1, 2]]);
geometry.updateBoundingBox();

var skeleton = new Skeleton();
skeleton.joints = [0, 1].map(function (index) {
    var node = new Node();
    node.update();
    return new Joint({index: index, node: node});
});
skeleton.updateJointMatrices();
// Move the second joint ten units along x in the current pose.
skeleton.joints[1].node.position.set(10, 0, 0);
skeleton.joints[1].node.update();
skeleton.updateJointsBoundingBoxes(geometry);
skeleton.update();

function makeMesh(offsetX) {
    var mesh = new Mesh({
        geometry: geometry,
        material: {shader: {}, transparent: false},
        culling: false
    });
    mesh.skeleton = skeleton;
    // Local joint indices: palette slot 0 -> skeleton joint 0, slot 1 -> joint 1.
    mesh.joints = [0, 1];
    mesh.offsetMatrix = new Matrix4();
    mesh.offsetMatrix.array[12] = offsetX;
    return mesh;
}

var scene = new Scene();
var left = makeMesh(-20);
var right = makeMesh(20);
scene.add(left);
scene.add(right);
scene.update();

// Per-instance CPU world data, recomputed on every call. The shared geometry
// (position, boundingBox, skinnedPosition) is never modified.
console.log('right world positions:', Array.from(right.getSkinnedWorldPositions()));
var box = right.getSkinnedWorldBoundingBox();
console.log('right bbox min:', Array.from(box.min.array), 'max:', Array.from(box.max.array));

// A camera centered on the +20 instance: the render list culls the other one
// and ray picking returns the exact instance, triangle and world point.
var camera = new Perspective({near: 0.01, far: 100, aspect: 1, fov: 60});
camera.position.set(30.25, 0.25, 2);
camera.lookAt(new Vector3(30.25, 0.25, 0));
camera.update();

var list = scene.updateRenderList(camera);
console.log('visible instances:', list.opaque.length, 'is right:', list.opaque[0] === right);

var rendererStub = {
    _height: 100,
    viewport: {x: 0, y: 0, width: 100, height: 100},
    screenToNDC: Renderer.prototype.screenToNDC
};
var hit = new RayPicking({scene: scene, camera: camera, renderer: rendererStub}).pick(50, 50);
console.log('picked target is right:', hit.target === right);
console.log('triangleIndex:', hit.triangleIndex);
console.log('pointWorld:', Array.from(hit.pointWorld.array));
console.log('point (before offset):', Array.from(hit.point.array));
console.log('distance from ray origin:', hit.distance);
