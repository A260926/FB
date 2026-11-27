/**
 * Regression suite for skinned mesh CPU skinning: culling, picking and the
 * local joint palette now share the same skinned space as rendering.
 *
 * What is covered (all expectations are computed with independent scalar
 * math in this file, never from the API under test):
 *
 *  - Mesh#getSkinnedWorldPositions / Mesh#getSkinnedWorldBoundingBox deform
 *    vertices with the current skeleton pose, the mesh local joint palette
 *    and offsetMatrix, following the shader weight rule (first weight always
 *    added, the other three only when > 1e-4, fourth weight = 1 - x - y - z,
 *    homogeneous divide at the very end).
 *  - Scene#updateRenderList culls every skinned instance with its own
 *    deformed bounding box instead of the shared skeleton box.
 *  - RayPicking broad phase and per-triangle intersection use the same
 *    instance world points; `point` of a skinned hit is the deformed
 *    intersection before offsetMatrix, `distance` is measured from the
 *    origin of the cast ray.
 *  - Skeleton#getSubSkinMatrices follows palette grow/shrink/reorder.
 *  - Shared geometry is never written (position, boundingBox and no
 *    skinnedPosition attribute), interleaved queries of instances sharing
 *    one geometry don't overwrite each other.
 *  - The plain non-skinned path, ignorePicking and custom
 *    geometry.pick / geometry.pickByRay keep their behaviour.
 *
 * Usage:
 *   sh task_support/run.sh target     # runs this suite through the runner
 *   node --permission --allow-fs-read='*' --no-addons --no-warnings \
 *        --import ./task_support/bootstrap.mjs test/skin-world.mjs
 *                                     # runs this file directly
 *
 * Every case is `{name, run(check)}`; `run` asserts only through the
 * provided `check(condition, message)` and returns nothing.
 */
import Geometry from '../src/Geometry.js';
import Scene from '../src/Scene.js';
import Node from '../src/Node.js';
import Joint from '../src/Joint.js';
import Skeleton from '../src/Skeleton.js';
import Matrix4 from '../src/math/Matrix4.js';
import {triangle, mesh, cameraAt, picker, rig} from '../task_support/fixtures.mjs';

// ---------------------------------------------------------------------------
// Independent scalar helpers. Plain double precision number math, deliberately
// not using any of the implementation under test.
// ---------------------------------------------------------------------------

// Column major 4x4 multiply, out = a * b.
function mat4Mul(a, b) {
    var out = new Array(16);
    for (var c = 0; c < 4; c++) {
        for (var r = 0; r < 4; r++) {
            var sum = 0;
            for (var k = 0; k < 4; k++) {
                sum += a[k * 4 + r] * b[c * 4 + k];
            }
            out[c * 4 + r] = sum;
        }
    }
    return out;
}

function translation(x, y, z) {
    return [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, x, y, z, 1];
}

function scaling(x, y, z) {
    return [x, 0, 0, 0, 0, y, 0, 0, 0, 0, z, 0, 0, 0, 0, 1];
}

function applyMat4(m, v) {
    return [
        m[0] * v[0] + m[4] * v[1] + m[8] * v[2] + m[12] * v[3],
        m[1] * v[0] + m[5] * v[1] + m[9] * v[2] + m[13] * v[3],
        m[2] * v[0] + m[6] * v[1] + m[10] * v[2] + m[14] * v[3],
        m[3] * v[0] + m[7] * v[1] + m[11] * v[2] + m[15] * v[3]
    ];
}

// World matrix of a rotation free joint node (transform order: scale, then
// translate, same as clay.Node#updateLocalTransform without rotation).
function trsMatrix(t, s) {
    return mat4Mul(translation(t[0], t[1], t[2]), scaling(s[0], s[1], s[2]));
}

function inverseTrsMatrix(t, s) {
    return mat4Mul(
        scaling(1 / s[0], 1 / s[1], 1 / s[2]),
        translation(-t[0], -t[1], -t[2])
    );
}

// Reference implementation of the documented skinning rule, computed from
// the raw geometry attributes and the given skin matrices (one 16 floats
// column major matrix per skeleton joint). `palette` maps local joint
// indices (the joint attribute) to skeleton joints.
function refSkinnedWorldPositions(geometry, palette, skinMatrices, offset) {
    var positionAttr = geometry.attributes.position;
    var weightAttr = geometry.attributes.weight;
    var jointAttr = geometry.attributes.joint;
    var pos = [];
    var weight = [];
    var joint = [];
    var out = [];
    for (var i = 0; i < geometry.vertexCount; i++) {
        positionAttr.get(i, pos);
        weightAttr.get(i, weight);
        jointAttr.get(i, joint);
        var h = [0, 0, 0, 0];
        for (var k = 0; k < 4; k++) {
            var w = k < 3 ? weight[k] : 1 - weight[0] - weight[1] - weight[2];
            if (k > 0 && w <= 1e-4) {
                continue;
            }
            var p = applyMat4(skinMatrices[palette[joint[k]]], [pos[0], pos[1], pos[2], 1]);
            h[0] += w * p[0];
            h[1] += w * p[1];
            h[2] += w * p[2];
            h[3] += w * p[3];
        }
        var g = offset ? applyMat4(offset, h) : h;
        out.push([g[0] / g[3], g[1] / g[3], g[2] / g[3]]);
    }
    return out;
}

function refBoundingBox(points) {
    var min = [Infinity, Infinity, Infinity];
    var max = [-Infinity, -Infinity, -Infinity];
    for (var i = 0; i < points.length; i++) {
        for (var k = 0; k < 3; k++) {
            min[k] = Math.min(min[k], points[i][k]);
            max[k] = Math.max(max[k], points[i][k]);
        }
    }
    return {min: min, max: max};
}

// ---------------------------------------------------------------------------
// Fixture builders on top of task_support/fixtures.mjs
// ---------------------------------------------------------------------------

// Geometry with per vertex weights/joints.
function skinnedGeometry(positions, weights, joints, indices) {
    var g = new Geometry();
    g.attributes.position.fromArray(positions);
    g.attributes.weight.fromArray(weights);
    g.attributes.joint.fromArray(joints);
    g.initIndicesFromArray(indices);
    g.updateBoundingBox();
    return g;
}

// Skeleton whose joints are rotation free nodes. `poses` is an array of
// {bind: {t, s}, current: {t, s}}. Also returns the reference skin matrices
// (current world matrix * inverse bind matrix) computed with scalar math.
function makeSkeleton(poses) {
    var skeleton = new Skeleton();
    skeleton.joints = poses.map(function (pose, index) {
        var node = new Node();
        node.position.set(pose.bind.t[0], pose.bind.t[1], pose.bind.t[2]);
        node.scale.set(pose.bind.s[0], pose.bind.s[1], pose.bind.s[2]);
        return new Joint({index: index, node: node});
    });
    for (var i = 0; i < skeleton.joints.length; i++) {
        skeleton.joints[i].node.update();
    }
    skeleton.updateJointMatrices();
    for (var i = 0; i < poses.length; i++) {
        var node = skeleton.joints[i].node;
        node.position.set(poses[i].current.t[0], poses[i].current.t[1], poses[i].current.t[2]);
        node.scale.set(poses[i].current.s[0], poses[i].current.s[1], poses[i].current.s[2]);
        node.update();
    }
    skeleton.update();
    var skinMatrices = poses.map(function (pose) {
        return mat4Mul(trsMatrix(pose.current.t, pose.current.s), inverseTrsMatrix(pose.bind.t, pose.bind.s));
    });
    return {skeleton: skeleton, skinMatrices: skinMatrices};
}

function skinnedMesh(geometry, skeleton, joints, offset) {
    var m = mesh(geometry);
    m.skeleton = skeleton;
    m.joints = joints;
    if (offset) {
        m.offsetMatrix = new Matrix4();
        for (var i = 0; i < 16; i++) {
            m.offsetMatrix.array[i] = offset[i];
        }
    }
    return m;
}

// ---------------------------------------------------------------------------
// Assertion helpers, all funnel through the provided check.
// ---------------------------------------------------------------------------

function expectClose(check, actual, expected, label) {
    var tolerance = 1e-4 * Math.max(1, Math.abs(expected));
    check(
        Math.abs(actual - expected) <= tolerance,
        label + ': expected ' + expected + ', got ' + actual
    );
}

function expectVec3(check, actual, expected, label) {
    for (var i = 0; i < 3; i++) {
        expectClose(check, actual[i], expected[i], label + '[' + i + ']');
    }
}

function expectPositions(check, actual, expected, label) {
    check(actual.length === expected.length * 3, label + ': vertex count matches');
    for (var i = 0; i < expected.length; i++) {
        expectVec3(check, [actual[i * 3], actual[i * 3 + 1], actual[i * 3 + 2]], expected[i], label + ' vertex ' + i);
    }
}

function expectBBox(check, bbox, expected, label) {
    expectVec3(check, bbox.min.array, expected.min, label + ' min');
    expectVec3(check, bbox.max.array, expected.max, label + ' max');
}

// ---------------------------------------------------------------------------
// Cases
// ---------------------------------------------------------------------------

var cases = [
    {
        // The baseline scenario: two instances share skeleton and geometry,
        // offsetMatrix moves them +20/-20. Culling and picking must use the
        // deformed bounds of each instance, not the shared skeleton box.
        name: 'offset-instances-share-skeleton',
        run: function (check) {
            var fixture = rig();
            var scene = fixture.scene;
            var skeleton = fixture.skeleton;
            var geometry = fixture.geometry;
            var a = fixture.a;
            var b = fixture.b;

            // Joint 3 sits at (10, 0, 0) with identity inverse bind, the
            // geometry weights select it through the fourth weight, so the
            // deformed triangle spans x in [10, 11]; offsets move it to
            // [30, 31] (a) and [-10, -9] (b).
            var camera = cameraAt(30.25);
            var list = scene.updateRenderList(camera);
            check(list.opaque.length === 1 && list.opaque[0] === a, 'only the +20 offset instance is listed');
            check(!list.opaque.includes(b), 'the -20 offset instance is culled for this camera');
            check(list.transparent.length === 0, 'transparent classification untouched');

            var otherCamera = cameraAt(-9.75);
            var otherList = scene.updateRenderList(otherCamera);
            check(otherList.opaque.length === 1 && otherList.opaque[0] === b, 'only the -20 offset instance is listed for the other camera');

            var hits = picker(scene, camera).pickAll(50, 50);
            check(hits.length === 1, 'exactly one hit through the offset instance');
            check(hits[0].target === a, 'hit target is the +20 offset instance');
            check(hits[0].triangleIndex === 0, 'hit triangle index');
            check(hits[0].triangle.join(',') === '0,1,2', 'hit triangle vertex indices');
            expectVec3(check, hits[0].pointWorld.array, [30.25, 0.25, 0], 'hit pointWorld');
            // point is the deformed intersection before offsetMatrix.
            expectVec3(check, hits[0].point.array, [10.25, 0.25, 0], 'hit point before offset');
            // Ray origin is the near plane point (30.25, 0.25, 2 - 0.01).
            expectClose(check, hits[0].distance, 1.99, 'hit distance from ray origin');

            var hitsB = picker(scene, otherCamera).pickAll(50, 50);
            check(hitsB.length === 1 && hitsB[0].target === b, 'the -20 offset instance is pickable at its own place');
            expectVec3(check, hitsB[0].pointWorld.array, [-9.75, 0.25, 0], 'hit pointWorld of b');

            // New API, expected values from scalar math.
            expectPositions(check, a.getSkinnedWorldPositions(), [[30, 0, 0], [31, 0, 0], [30, 1, 0]], 'instance a');
            expectPositions(check, b.getSkinnedWorldPositions(), [[-10, 0, 0], [-9, 0, 0], [-10, 1, 0]], 'instance b');
            expectBBox(check, a.getSkinnedWorldBoundingBox(), {min: [30, 0, 0], max: [31, 1, 0]}, 'instance a box');
            expectBBox(check, b.getSkinnedWorldBoundingBox(), {min: [-10, 0, 0], max: [-9, 1, 0]}, 'instance b box');

            // The shared geometry must not be written by any query.
            check(!('skinnedPosition' in geometry.attributes), 'no skinnedPosition attribute is created');
            check(
                Array.prototype.join.call(geometry.attributes.position.value, ',') === '0,0,0,1,0,0,0,1,0',
                'shared position attribute is unchanged'
            );
            expectVec3(check, geometry.boundingBox.min.array, [0, 0, 0], 'shared geometry bounding box min');
            expectVec3(check, geometry.boundingBox.max.array, [1, 1, 0], 'shared geometry bounding box max');

            // Palette of the shared skeleton follows length changes.
            check(skeleton.getSubSkinMatrices('baseline', [0]).length === 16, 'palette of one joint');
            check(skeleton.getSubSkinMatrices('baseline', [0, 1, 2, 3]).length === 64, 'palette grown to four joints');
        }
    },
    {
        // Non-identity inverse bind matrices, non-uniform scale, reflection
        // in a joint matrix and a shearing offsetMatrix.
        name: 'non-identity-inverse-bind-affine',
        run: function (check) {
            var rig2 = makeSkeleton([
                // Joint 0: bound at (5,0,0) scale (2,1,1), posed at (7,1,0)
                // scale (4,1,1). skin0: x' = 2x - 3, y' = y + 1, z' = z.
                {bind: {t: [5, 0, 0], s: [2, 1, 1]}, current: {t: [7, 1, 0], s: [4, 1, 1]}},
                // Joint 1: bound at origin, posed with reflection and
                // translation. skin1: x' = -2x, y' = y, z' = z - 4.
                {bind: {t: [0, 0, 0], s: [1, 1, 1]}, current: {t: [0, 0, -4], s: [-2, 1, 1]}}
            ]);
            var geometry = skinnedGeometry(
                [[0, 0, 0], [1, 0, 0], [0, 1, 0]],
                [[1, 0, 0], [0.25, 0.75, 0], [0, 0, 0]],
                [[0, 1, 0, 1], [0, 1, 0, 1], [0, 1, 0, 1]],
                [[0, 1, 2]]
            );
            // Shear x by 0.5 * y and translate x by 10.
            var offset = translation(10, 0, 0);
            offset[4] = 0.5;
            var object = skinnedMesh(geometry, rig2.skeleton, [0, 1], offset);
            var scene = new Scene();
            scene.add(object);
            scene.update();

            var expected = refSkinnedWorldPositions(geometry, [0, 1], rig2.skinMatrices, offset);
            // Scalar anchor values of the reference computation:
            // deformed: (-3,1,0), (-1.75,0.25,-3), (0,1,-4)
            // world:    (7.5,1,0), (8.375,0.25,-3), (10.5,1,-4)
            expectVec3(check, expected[0], [7.5, 1, 0], 'reference world vertex 0');
            expectVec3(check, expected[1], [8.375, 0.25, -3], 'reference world vertex 1');
            expectVec3(check, expected[2], [10.5, 1, -4], 'reference world vertex 2');

            expectPositions(check, object.getSkinnedWorldPositions(), expected, 'affine skinning');
            expectBBox(check, object.getSkinnedWorldBoundingBox(), refBoundingBox(expected), 'affine skinning box');
            expectVec3(check, object.getSkinnedWorldBoundingBox().min.array, [7.5, 0.25, -4], 'box min anchor');
            expectVec3(check, object.getSkinnedWorldBoundingBox().max.array, [10.5, 1, 0], 'box max anchor');

            var camera = cameraAt(8.5, 0.6, 2);
            var list = scene.updateRenderList(camera);
            check(list.opaque.length === 1 && list.opaque[0] === object, 'affine deformed instance is listed');
            var farList = scene.updateRenderList(cameraAt(108.5, 0.6, 2));
            check(!farList.opaque.includes(object), 'affine deformed instance is culled far away');

            // Expected intersection: the vertical ray through (8.5, 0.6)
            // hits the plane of the three world vertices. Solve the plane
            // equation with scalar math.
            var a = expected[0];
            var b = expected[1];
            var c = expected[2];
            var ab = [b[0] - a[0], b[1] - a[1], b[2] - a[2]];
            var ac = [c[0] - a[0], c[1] - a[1], c[2] - a[2]];
            var n = [
                ab[1] * ac[2] - ab[2] * ac[1],
                ab[2] * ac[0] - ab[0] * ac[2],
                ab[0] * ac[1] - ab[1] * ac[0]
            ];
            var d = n[0] * a[0] + n[1] * a[1] + n[2] * a[2];
            var hitZ = (d - n[0] * 8.5 - n[1] * 0.6) / n[2];

            var hits = picker(scene, camera).pickAll(50, 50);
            check(hits.length === 1, 'one hit on the sheared instance');
            check(hits[0].target === object, 'hit target');
            expectVec3(check, hits[0].pointWorld.array, [8.5, 0.6, hitZ], 'hit pointWorld on sheared instance');
            // point is before the offset: invert x' = x + 0.5y + 10.
            expectVec3(check, hits[0].point.array, [8.5 - 0.5 * 0.6 - 10, 0.6, hitZ], 'hit point before shear offset');
            expectClose(check, hits[0].distance, 1.99 - hitZ, 'hit distance from ray origin');
        }
    },
    {
        // Weights on both sides of the 1e-4 threshold: only strictly larger
        // weights are added (except the unconditional first one), and the
        // homogeneous divide happens once at the end.
        name: 'weight-threshold',
        run: function (check) {
            var rig3 = makeSkeleton([
                {bind: {t: [0, 0, 0], s: [1, 1, 1]}, current: {t: [0, 0, 0], s: [1, 1, 1]}},
                {bind: {t: [0, 0, 0], s: [1, 1, 1]}, current: {t: [10, 0, 0], s: [1, 1, 1]}},
                {bind: {t: [0, 0, 0], s: [1, 1, 1]}, current: {t: [0, 100, 0], s: [1, 1, 1]}},
                {bind: {t: [0, 0, 0], s: [1, 1, 1]}, current: {t: [0, 0, 1000], s: [1, 1, 1]}}
            ]);
            var geometry = skinnedGeometry(
                [[0, 0, 0], [1, 0, 0], [0, 1, 0], [2, 0, 0]],
                [
                    // w1 = 9e-5 <= 1e-4: excluded, w3 ~ 0: excluded.
                    [0.99991, 9e-5, 0],
                    // w1 = 0.4999 > 1e-4: included, w3 ~ 1e-4: excluded.
                    [0.5, 0.4999, 0],
                    // w3 = 0.2 > 1e-4: included.
                    [0.5, 0.2, 0.1],
                    // w1 = 1.1e-4 > 1e-4: included, w3 ~ 9e-5: excluded.
                    [0.9998, 1.1e-4, 0]
                ],
                [[1, 2, 0, 3], [1, 2, 0, 3], [1, 2, 0, 3], [1, 2, 0, 3]],
                [[0, 1, 2]]
            );
            var object = skinnedMesh(geometry, rig3.skeleton, [0, 1, 2, 3], null);
            var scene = new Scene();
            scene.add(object);
            scene.update();

            var expected = refSkinnedWorldPositions(geometry, [0, 1, 2, 3], rig3.skinMatrices, null);
            expectPositions(check, object.getSkinnedWorldPositions(), expected, 'threshold skinning');

            // Scalar anchors: the discriminating components.
            // v0: only the first weight counts, renormalized to (10, 0, 0).
            expectVec3(check, expected[0], [10, 0, 0], 'threshold vertex 0');
            // v1: w3 ~ 1e-4 is excluded, z stays 0 (would be ~0.1 otherwise).
            expectVec3(check, expected[1], [5.9999 / 0.9999, 49.99 / 0.9999, 0], 'threshold vertex 1');
            // v2: w3 = 0.2 is included, z becomes 200.
            expectVec3(check, expected[2], [5, 21, 200], 'threshold vertex 2');
            // v3: w1 = 1.1e-4 is included, y is ~0.011 (would be 0 otherwise).
            check(expected[3][1] > 0.01, 'reference vertex 3 includes the 1.1e-4 weight');
            check(expected[3][2] === 0, 'reference vertex 3 excludes w3 ~ 9e-5');

            expectBBox(check, object.getSkinnedWorldBoundingBox(), refBoundingBox(expected), 'threshold box');

            // Pick the first triangle at barycentric (0.5, 0.25, 0.25).
            var p = [
                0.5 * expected[0][0] + 0.25 * expected[1][0] + 0.25 * expected[2][0],
                0.5 * expected[0][1] + 0.25 * expected[1][1] + 0.25 * expected[2][1],
                0.5 * expected[0][2] + 0.25 * expected[1][2] + 0.25 * expected[2][2]
            ];
            var camera = cameraAt(p[0], p[1], p[2] + 2);
            var list = scene.updateRenderList(camera);
            check(list.opaque.length === 1 && list.opaque[0] === object, 'threshold instance is listed');
            var hits = picker(scene, camera).pickAll(50, 50);
            check(hits.length === 1, 'one hit on the threshold triangle');
            check(hits[0].target === object && hits[0].triangleIndex === 0, 'threshold hit target and triangle');
            expectVec3(check, hits[0].pointWorld.array, p, 'threshold hit pointWorld');
            // No offsetMatrix: point equals pointWorld.
            expectVec3(check, hits[0].point.array, p, 'threshold hit point');
            expectClose(check, hits[0].distance, 1.99, 'threshold hit distance');
        }
    },
    {
        // Same frame edits take effect on the very next query: bone moved,
        // offsetMatrix changed, palette reordered, geometry swapped. The
        // query itself never advances the animation.
        name: 'same-frame-updates',
        run: function (check) {
            var rig4 = makeSkeleton([
                {bind: {t: [0, 0, 0], s: [1, 1, 1]}, current: {t: [0, 0, 0], s: [1, 1, 1]}},
                {bind: {t: [0, 0, 0], s: [1, 1, 1]}, current: {t: [0, 0, 0], s: [1, 1, 1]}}
            ]);
            var skeleton = rig4.skeleton;
            var geometry = skinnedGeometry(
                [[0, 0, 0], [1, 0, 0], [0, 1, 0]],
                [[0, 0, 0], [0, 0, 0], [0, 0, 0]],
                [[0, 0, 0, 1], [0, 0, 0, 1], [0, 0, 0, 1]],
                [[0, 1, 2]]
            );
            var object = skinnedMesh(geometry, skeleton, [0, 1], null);
            var scene = new Scene();
            scene.add(object);
            scene.update();

            var camera = cameraAt(0.25, 0.25, 2);
            check(scene.updateRenderList(camera).opaque[0] === object, 'listed at the rest pose');
            var hits = picker(scene, camera).pickAll(50, 50);
            check(hits.length === 1 && hits[0].target === object, 'picked at the rest pose');
            expectVec3(check, hits[0].pointWorld.array, [0.25, 0.25, 0], 'rest pose hit');

            // Move the bone within the same frame; the caller updates the
            // node and the skeleton as usual.
            skeleton.joints[1].node.position.set(50, 0, 0);
            skeleton.joints[1].node.update();
            skeleton.update();
            expectPositions(check, object.getSkinnedWorldPositions(), [[50, 0, 0], [51, 0, 0], [50, 1, 0]], 'moved bone');
            check(!scene.updateRenderList(camera).opaque.includes(object), 'culled at the old place');
            check(picker(scene, camera).pickAll(50, 50).length === 0, 'not pickable at the old place');
            var movedCamera = cameraAt(50.25, 0.25, 2);
            check(scene.updateRenderList(movedCamera).opaque[0] === object, 'listed at the moved place');
            var movedHits = picker(scene, movedCamera).pickAll(50, 50);
            check(movedHits.length === 1 && movedHits[0].target === object, 'picked at the moved place');
            expectVec3(check, movedHits[0].pointWorld.array, [50.25, 0.25, 0], 'moved hit');

            // Change offsetMatrix in the same frame, no skeleton update.
            object.offsetMatrix = new Matrix4();
            object.offsetMatrix.array[12] = -50;
            expectPositions(check, object.getSkinnedWorldPositions(), [[0, 0, 0], [1, 0, 0], [0, 1, 0]], 'offset applied immediately');
            check(scene.updateRenderList(camera).opaque[0] === object, 'listed again after offset change');
            var offsetHits = picker(scene, camera).pickAll(50, 50);
            check(offsetHits.length === 1, 'picked again after offset change');
            expectVec3(check, offsetHits[0].pointWorld.array, [0.25, 0.25, 0], 'offset hit pointWorld');
            // The deformed intersection before the -50 offset.
            expectVec3(check, offsetHits[0].point.array, [50.25, 0.25, 0], 'offset hit point before offset');

            // Reorder the palette in the same frame: local joint 1 now maps
            // to skeleton joint 0 (identity), offset still applies.
            object.joints = [1, 0];
            expectPositions(check, object.getSkinnedWorldPositions(), [[-50, 0, 0], [-49, 0, 0], [-50, 1, 0]], 'reordered palette');
            var reorderedCamera = cameraAt(-49.75, 0.25, 2);
            check(scene.updateRenderList(reorderedCamera).opaque[0] === object, 'listed after palette reorder');
            check(picker(scene, reorderedCamera).pickAll(50, 50).length === 1, 'picked after palette reorder');

            // Swap the geometry in the same frame.
            object.geometry = skinnedGeometry(
                [[100, 100, 0], [101, 100, 0], [100, 101, 0]],
                [[0, 0, 0], [0, 0, 0], [0, 0, 0]],
                [[0, 0, 0, 1], [0, 0, 0, 1], [0, 0, 0, 1]],
                [[0, 1, 2]]
            );
            expectPositions(check, object.getSkinnedWorldPositions(), [[50, 100, 0], [51, 100, 0], [50, 101, 0]], 'swapped geometry');
            var swappedCamera = cameraAt(50.25, 100.25, 2);
            check(scene.updateRenderList(swappedCamera).opaque[0] === object, 'listed after geometry swap');
            var swappedHits = picker(scene, swappedCamera).pickAll(50, 50);
            check(swappedHits.length === 1 && swappedHits[0].target === object, 'picked after geometry swap');
            expectVec3(check, swappedHits[0].pointWorld.array, [50.25, 100.25, 0], 'swapped geometry hit');
        }
    },
    {
        // Skeleton#getSubSkinMatrices returns length and content matching
        // the requested palette when it grows, shrinks or is reordered.
        name: 'palette-grow-shrink-reorder',
        run: function (check) {
            var rig5 = makeSkeleton([
                {bind: {t: [0, 0, 0], s: [1, 1, 1]}, current: {t: [1, 0, 0], s: [1, 1, 1]}},
                {bind: {t: [0, 0, 0], s: [1, 1, 1]}, current: {t: [0, 2, 0], s: [1, 1, 1]}},
                {bind: {t: [0, 0, 0], s: [1, 1, 1]}, current: {t: [0, 0, 3], s: [1, 1, 1]}}
            ]);
            var skeleton = rig5.skeleton;
            var skinMatrices = rig5.skinMatrices;
            var meshId = 'palette-case';

            function expectBlock(sub, block, jointIndex, label) {
                for (var i = 0; i < 16; i++) {
                    expectClose(check, sub[block * 16 + i], skinMatrices[jointIndex][i], label + '[' + i + ']');
                }
            }

            var sub = skeleton.getSubSkinMatrices(meshId, [0]);
            check(sub.length === 16, 'single joint palette length');
            expectBlock(sub, 0, 0, 'single joint palette content');

            sub = skeleton.getSubSkinMatrices(meshId, [0, 1, 2]);
            check(sub.length === 48, 'grown palette length');
            expectBlock(sub, 0, 0, 'grown palette block 0');
            expectBlock(sub, 1, 1, 'grown palette block 1');
            expectBlock(sub, 2, 2, 'grown palette block 2');

            sub = skeleton.getSubSkinMatrices(meshId, [2, 0]);
            check(sub.length === 32, 'reordered palette length');
            expectBlock(sub, 0, 2, 'reordered palette block 0 is joint 2');
            expectBlock(sub, 1, 0, 'reordered palette block 1 is joint 0');

            sub = skeleton.getSubSkinMatrices(meshId, [1]);
            check(sub.length === 16, 'shrunk palette length');
            expectBlock(sub, 0, 1, 'shrunk palette content');

            // Mesh level: reordering Mesh#joints retargets the local
            // indices immediately, visible through culling and picking.
            var geometry = skinnedGeometry(
                [[0, 0, 0], [1, 0, 0], [0, 1, 0]],
                [[0, 0, 0], [0, 0, 0], [0, 0, 0]],
                [[0, 0, 0, 0], [0, 0, 0, 0], [0, 0, 0, 0]],
                [[0, 1, 2]]
            );
            var object = skinnedMesh(geometry, skeleton, [0, 1, 2], null);
            var scene = new Scene();
            scene.add(object);
            scene.update();

            // Local joint 0 -> skeleton joint 0 -> translation (1, 0, 0).
            expectPositions(check, object.getSkinnedWorldPositions(), [[1, 0, 0], [2, 0, 0], [1, 1, 0]], 'palette order 0,1,2');
            var cameraA = cameraAt(1.25, 0.25, 2);
            check(scene.updateRenderList(cameraA).opaque[0] === object, 'listed with palette 0,1,2');
            check(picker(scene, cameraA).pickAll(50, 50).length === 1, 'picked with palette 0,1,2');

            // Local joint 0 -> skeleton joint 2 -> translation (0, 0, 3).
            object.joints = [2, 1, 0];
            expectPositions(check, object.getSkinnedWorldPositions(), [[0, 0, 3], [1, 0, 3], [0, 1, 3]], 'palette order 2,1,0');
            check(!scene.updateRenderList(cameraA).opaque.includes(object), 'culled after palette reorder');
            var cameraB = cameraAt(0.25, 0.25, 5);
            check(scene.updateRenderList(cameraB).opaque[0] === object, 'listed after palette reorder');
            var hits = picker(scene, cameraB).pickAll(50, 50);
            check(hits.length === 1 && hits[0].target === object, 'picked after palette reorder');
            expectVec3(check, hits[0].pointWorld.array, [0.25, 0.25, 3], 'hit after palette reorder');
        }
    },
    {
        // Two instances share one geometry; interleaved queries and picks
        // never overwrite each other nor the shared geometry.
        name: 'shared-geometry-interleaved-queries',
        run: function (check) {
            var fixture = rig();
            var scene = fixture.scene;
            var geometry = fixture.geometry;
            var a = fixture.a;
            var b = fixture.b;

            var pa = a.getSkinnedWorldPositions();
            var snapshotA = Array.prototype.slice.call(pa);
            var pb = b.getSkinnedWorldPositions();
            var snapshotB = Array.prototype.slice.call(pb);

            // Querying b did not disturb the result a holds.
            check(pa !== pb, 'each query returns its own array');
            expectPositions(check, pa, [[30, 0, 0], [31, 0, 0], [30, 1, 0]], 'a positions after b query');
            check(snapshotA.join(',') === Array.prototype.join.call(pa, ','), 'a result array is stable');

            // Interleave picks of both instances.
            var cameraA = cameraAt(30.25);
            var cameraB = cameraAt(-9.75);
            var pickA = picker(scene, cameraA);
            var pickB = picker(scene, cameraB);
            var hitsA1 = pickA.pickAll(50, 50);
            var hitsB1 = pickB.pickAll(50, 50);
            var hitsA2 = pickA.pickAll(50, 50);
            check(hitsA1.length === 1 && hitsA1[0].target === a, 'first pick hits a');
            check(hitsB1.length === 1 && hitsB1[0].target === b, 'interleaved pick hits b');
            check(hitsA2.length === 1 && hitsA2[0].target === a, 'second pick hits a again');
            expectVec3(check, hitsB1[0].pointWorld.array, [-9.75, 0.25, 0], 'b hit pointWorld');
            // Both instances share the same deformed space before offset.
            expectVec3(check, hitsA1[0].point.array, [10.25, 0.25, 0], 'a hit point before offset');
            expectVec3(check, hitsB1[0].point.array, [10.25, 0.25, 0], 'b hit point before offset');

            // Interleaved bounding boxes stay independent.
            var boxA = a.getSkinnedWorldBoundingBox();
            var boxB = b.getSkinnedWorldBoundingBox();
            check(boxA !== boxB, 'each query returns its own bounding box');
            expectBBox(check, boxA, {min: [30, 0, 0], max: [31, 1, 0]}, 'a box after b box query');
            expectBBox(check, boxB, {min: [-10, 0, 0], max: [-9, 1, 0]}, 'b box');
            expectBBox(check, a.getSkinnedWorldBoundingBox(), {min: [30, 0, 0], max: [31, 1, 0]}, 'a box queried again');

            // Re-querying positions still matches the first snapshot.
            check(
                Array.prototype.join.call(a.getSkinnedWorldPositions(), ',') === snapshotA.join(','),
                'a positions reproducible'
            );
            check(
                Array.prototype.join.call(b.getSkinnedWorldPositions(), ',') === snapshotB.join(','),
                'b positions reproducible'
            );

            // The shared geometry was never written.
            check(!('skinnedPosition' in geometry.attributes), 'no skinnedPosition attribute is created');
            check(
                Array.prototype.join.call(geometry.attributes.position.value, ',') === '0,0,0,1,0,0,0,1,0',
                'shared position attribute is unchanged'
            );
            expectVec3(check, geometry.boundingBox.min.array, [0, 0, 0], 'shared geometry box min');
            expectVec3(check, geometry.boundingBox.max.array, [1, 1, 0], 'shared geometry box max');

            // And the render list still separates both instances.
            check(scene.updateRenderList(cameraA).opaque[0] === a, 'a listed for its camera');
            check(scene.updateRenderList(cameraB).opaque[0] === b, 'b listed for its camera');
        }
    },
    {
        // The plain non-skinned path, the frustumCulling switch,
        // ignorePicking and the custom geometry.pick / geometry.pickByRay
        // hooks keep their original behaviour.
        name: 'static-and-custom-picking',
        run: function (check) {
            var scene = new Scene();

            // Static mesh with a world transform.
            var staticMesh = mesh(triangle());
            staticMesh.position.set(5, 0, 0);
            scene.add(staticMesh);

            // Offscreen static mesh with frustumCulling disabled.
            var noCullMesh = mesh(triangle());
            noCullMesh.position.set(1000, 0, 0);
            noCullMesh.frustumCulling = false;
            scene.add(noCullMesh);

            // Offscreen skinned mesh with frustumCulling disabled.
            var fixture = rig();
            var skinnedNoCull = fixture.a;
            skinnedNoCull.frustumCulling = false;
            scene.add(skinnedNoCull);

            scene.update();

            var camera = cameraAt(5.25, 0.25, 2);
            var list = scene.updateRenderList(camera);
            check(list.opaque.includes(staticMesh), 'static mesh listed');
            check(list.opaque.includes(noCullMesh), 'frustumCulling=false keeps offscreen mesh listed');
            check(list.opaque.includes(skinnedNoCull), 'frustumCulling=false keeps offscreen skinned mesh listed');
            skinnedNoCull.frustumCulling = true;
            check(!scene.updateRenderList(camera).opaque.includes(skinnedNoCull), 'frustumCulling=true culls the skinned mesh again');

            var hits = picker(scene, camera).pickAll(50, 50);
            check(hits.length === 1 && hits[0].target === staticMesh, 'static mesh picked');
            expectVec3(check, hits[0].pointWorld.array, [5.25, 0.25, 0], 'static hit pointWorld');
            expectVec3(check, hits[0].point.array, [0.25, 0.25, 0], 'static hit point in local space');
            expectClose(check, hits[0].distance, 1.99, 'static hit distance');
            check(hits[0].triangleIndex === 0, 'static hit triangle index');

            // ignorePicking is honoured unless forced.
            staticMesh.ignorePicking = true;
            check(picker(scene, camera).pickAll(50, 50).length === 0, 'ignorePicking skips the mesh');
            var forced = picker(scene, camera).pickAll(50, 50, [], true);
            check(forced.length === 1 && forced[0].target === staticMesh, 'forcePickAll overrides ignorePicking');
            staticMesh.ignorePicking = false;

            // Custom pickByRay replaces the triangle intersection.
            var rayMesh = mesh(triangle());
            scene.add(rayMesh);
            scene.update();
            var rayCalls = [];
            rayMesh.geometry.pickByRay = function (ray, renderable, out) {
                rayCalls.push([ray, renderable, out]);
                out.push({distance: 1, target: renderable, custom: 'pickByRay'});
            };
            var centerCamera = cameraAt(0.25, 0.25, 2);
            var rayHits = picker(scene, centerCamera).pickAll(50, 50);
            check(rayCalls.length === 1, 'pickByRay called once');
            check(rayCalls[0][1] === rayMesh, 'pickByRay receives the renderable');
            check(Array.isArray(rayCalls[0][2]), 'pickByRay receives the output array');
            check(rayHits.some(function (h) { return h.custom === 'pickByRay' && h.target === rayMesh; }), 'pickByRay output is collected');

            // Custom pick (NDC based) replaces the triangle intersection.
            var ndcMesh = mesh(triangle());
            scene.add(ndcMesh);
            scene.update();
            var pickCalls = [];
            ndcMesh.geometry.pick = function (x, y, renderer, camera2, renderable, out) {
                pickCalls.push([x, y, renderable]);
                out.push({distance: 2, target: renderable, custom: 'pick'});
            };
            var ndcHits = picker(scene, centerCamera).pickAll(50, 50);
            check(pickCalls.length === 1, 'pick called once');
            check(Math.abs(pickCalls[0][0]) < 1e-6 && Math.abs(pickCalls[0][1]) < 1e-6, 'pick receives NDC of the pixel');
            check(pickCalls[0][2] === ndcMesh, 'pick receives the renderable');
            check(ndcHits.some(function (h) { return h.custom === 'pick' && h.target === ndcMesh; }), 'pick output is collected');
        }
    }
];

export default cases;

// Runnable directly: node --import ./task_support/bootstrap.mjs test/skin-world.mjs
if (process.argv[1] && import.meta.url.endsWith(process.argv[1].replace(/\\/g, '/'))) {
    Promise.resolve().then(async function () {
        for (var i = 0; i < cases.length; i++) {
            var c = cases[i];
            var checks = 0;
            await c.run(function (condition, message) {
                checks++;
                if (!condition) {
                    throw new Error('[' + c.name + '] ' + message);
                }
            });
            console.log('CASE ' + c.name + ' checks=' + checks);
        }
        console.log('SKIN_WORLD_COMPLETE cases=' + cases.length);
    }).catch(function (err) {
        console.error(err);
        process.exitCode = 1;
    });
}
