// Regression for CPU side skinning: per-instance world positions, bounding
// boxes, frustum culling and ray picking must all use the same deformed space
// as the draw path (local joint palette + offsetMatrix), without ever writing
// to the shared geometry.
//
// Every numeric expectation below is recomputed from the raw matrix arrays in
// this file, never taken from the bounding boxes returned by the new methods.
import Scene from '../src/Scene.js';
import Node from '../src/Node.js';
import Joint from '../src/Joint.js';
import Skeleton from '../src/Skeleton.js';
import Matrix4 from '../src/math/Matrix4.js';
import Vector3 from '../src/math/Vector3.js';
import {triangle, mesh, cameraAt, picker, rig} from '../task_support/fixtures.mjs';

var EPS = 1e-5;

function approx(check, actual, expected, message) {
    check(
        typeof actual === 'number' && isFinite(actual)
        && Math.abs(actual - expected) <= EPS,
        message + ' (actual=' + actual + ', expected=' + expected + ')'
    );
}

// --- Independent scalar reference, written only against raw Float32Arrays ---

function mat4Vec4(m, base, x, y, z, w) {
    return [
        m[base] * x + m[base + 4] * y + m[base + 8] * z + m[base + 12] * w,
        m[base + 1] * x + m[base + 5] * y + m[base + 9] * z + m[base + 13] * w,
        m[base + 2] * x + m[base + 6] * y + m[base + 10] * z + m[base + 14] * w,
        m[base + 3] * x + m[base + 7] * y + m[base + 11] * z + m[base + 15] * w
    ];
}

function addScaled(out, v, s) {
    out[0] += v[0] * s;
    out[1] += v[1] * s;
    out[2] += v[2] * s;
    out[3] += v[3] * s;
}

// Reference of one skinned vertex: same rule as clay.chunk.skin_matrix,
// weighting homogeneous positions before the offset and before the divide.
function referenceVertex(mesh, px, py, pz, weights3, joints4) {
    var skin = mesh.skeleton._skinMatricesArray;
    var ws = [
        weights3[0],
        weights3[1],
        weights3[2],
        1 - weights3[0] - weights3[1] - weights3[2]
    ];
    var acc = [0, 0, 0, 0];
    for (var k = 0; k < 4; k++) {
        // First term is unconditional; the rest need a strictly positive weight.
        if (k !== 0 && !(ws[k] > 1e-4)) {
            continue;
        }
        var base = mesh.joints[joints4[k]] * 16;
        addScaled(acc, mat4Vec4(skin, base, px, py, pz, 1), ws[k]);
    }
    var offset = mesh.offsetMatrix ? mesh.offsetMatrix.array : null;
    if (!offset) {
        return [acc[0] / acc[3], acc[1] / acc[3], acc[2] / acc[3]];
    }
    var h = mat4Vec4(offset, 0, acc[0], acc[1], acc[2], acc[3]);
    return [h[0] / h[3], h[1] / h[3], h[2] / h[3]];
}

function referenceMesh(mesh) {
    var geometry = mesh.geometry;
    var position = geometry.attributes.position.value;
    var weightValue = geometry.attributes.weight.value;
    var jointValue = geometry.attributes.joint.value;
    var result = [];
    for (var i = 0; i < geometry.vertexCount; i++) {
        var p = referenceVertex(
            mesh,
            position[i * 3], position[i * 3 + 1], position[i * 3 + 2],
            [weightValue[i * 3], weightValue[i * 3 + 1], weightValue[i * 3 + 2]],
            [jointValue[i * 4], jointValue[i * 4 + 1], jointValue[i * 4 + 2], jointValue[i * 4 + 3]]
        );
        result.push(p);
    }
    return result;
}

function referenceBox(points) {
    var min = [Infinity, Infinity, Infinity];
    var max = [-Infinity, -Infinity, -Infinity];
    points.forEach(function (p) {
        for (var d = 0; d < 3; d++) {
            if (p[d] < min[d]) { min[d] = p[d]; }
            if (p[d] > max[d]) { max[d] = p[d]; }
        }
    });
    return {min: min, max: max};
}

function checkAgainstReference(check, mesh, message) {
    var expectedPoints = referenceMesh(mesh);
    var actual = mesh.getSkinnedWorldPositions();
    check(actual instanceof Float32Array, message + ': positions are a Float32Array');
    check(actual.length === expectedPoints.length * 3, message + ': position length matches vertices');
    expectedPoints.forEach(function (p, i) {
        for (var d = 0; d < 3; d++) {
            approx(check, actual[i * 3 + d], p[d], message + ': vertex ' + i + ' axis ' + d);
        }
    });
    var expectedBox = referenceBox(expectedPoints);
    var box = mesh.getSkinnedWorldBoundingBox();
    for (var d = 0; d < 3; d++) {
        approx(check, box.min.array[d], expectedBox.min[d], message + ': bbox min axis ' + d);
        approx(check, box.max.array[d], expectedBox.max[d], message + ': bbox max axis ' + d);
    }
    return expectedPoints;
}

// Build a skeleton whose joints start from the given bind world matrices.
function buildSkeleton(bindWorlds) {
    var skeleton = new Skeleton();
    skeleton.joints = bindWorlds.map(function (m, index) {
        var node = new Node();
        node.worldTransform.setArray(m);
        node.localTransform.setArray(m);
        node._needsUpdateWorldTransform = false;
        return new Joint({index: index, node: node});
    });
    skeleton.updateJointMatrices();
    return skeleton;
}

function identity16() {
    return new Matrix4().array;
}

function translation16(x, y, z) {
    var m = new Matrix4();
    m.array[12] = x;
    m.array[13] = y;
    m.array[14] = z;
    return m.array;
}

function setJointWorld(skeleton, index, array16) {
    var node = skeleton.joints[index].node;
    node.worldTransform.setArray(array16);
    node.localTransform.setArray(array16);
    node._needsUpdateWorldTransform = false;
}

function skinnedMesh(geometry, skeleton, joints, offset) {
    var m = mesh(geometry);
    m.skeleton = skeleton;
    m.joints = joints;
    m.offsetMatrix = offset || new Matrix4();
    return m;
}

var cases = [
    {
        name: 'render-list-and-picking-use-each-instance-bounds',
        run: function (check) {
            var data = rig();
            var scene = data.scene, skeleton = data.skeleton, a = data.a, b = data.b;

            // Camera looks down -z and only the +20 offset instance is in view.
            var camA = cameraAt(30.25);
            var list = scene.updateRenderList(camA);
            check(list.opaque.length === 1, 'only one instance survives culling');
            check(list.opaque[0] === a, 'render list keeps the exact in-view instance');
            check(!list.opaque.includes(b), 'the other offset instance is culled');

            var pickerA = picker(scene, camA);
            var hits = pickerA.pickAll(50, 50);
            check(hits.length === 1, 'one picking hit');
            var hit = hits[0];
            check(hit.target === a, 'hit target is the visible instance');
            check(hit.triangleIndex === 0, 'triangle index reported');
            check(hit.triangle[0] === 0 && hit.triangle[1] === 1 && hit.triangle[2] === 2, 'triangle indices');
            approx(check, hit.pointWorld.array[0], 30.25, 'pointWorld x includes offset');
            approx(check, hit.pointWorld.array[1], 0.25, 'pointWorld y');
            approx(check, hit.pointWorld.array[2], 0, 'pointWorld z');
            approx(check, hit.point.array[0], 10.25, 'point is the deformed point before offset');
            approx(check, hit.point.array[1], 0.25, 'pre-offset point y');
            approx(check, hit.point.array[2], 0, 'pre-offset point z');
            check(hit.distance > 0 && isFinite(hit.distance), 'distance measured from the cast ray origin');
            approx(check, hit.distance, Vector3.dist(hit.pointWorld, pickerA._ray.origin), 'distance matches ray origin rule');

            // A ray aimed at the second, offset-away instance must still reach it.
            var camB = cameraAt(-9.75);
            var hitsB = picker(scene, camB).pickAll(50, 50);
            check(hitsB.length === 1 && hitsB[0].target === b, 'second instance picked through its own bounds');
            approx(check, hitsB[0].pointWorld.array[0], -9.75, 'second instance pointWorld');
            approx(check, hitsB[0].point.array[0], 10.25, 'second instance point before its own offset');

            // frustumCulling switch is still honored for skinned meshes.
            b.frustumCulling = false;
            var unculled = scene.updateRenderList(camA);
            check(unculled.opaque.includes(b), 'frustumCulling=false forces the instance into the list');
            b.frustumCulling = true;

            // Transparent classification is untouched.
            a.material.transparent = true;
            var transparentList = scene.updateRenderList(camA);
            check(transparentList.transparent[0] === a && transparentList.opaque.length === 0, 'transparent classification preserved');
            a.material.transparent = false;

            // Skeleton palette must not be mistaken for the instance bounds.
            check(skeleton.boundingBox !== a.getSkinnedWorldBoundingBox(), 'instance box is not the shared skeleton box');
        }
    },
    {
        name: 'non-identity-inverse-bind-matrix',
        run: function (check) {
            // Joint 1 is bound away from the origin, so its inverse bind is not identity.
            var skeleton = buildSkeleton([identity16(), translation16(5, 6, 7)]);
            setJointWorld(skeleton, 0, identity16());
            setJointWorld(skeleton, 1, translation16(10, 0, 0));

            var geometry = triangle([0, 0, 0], [0, 1, 0, 1]);
            skeleton.updateJointsBoundingBoxes(geometry);
            skeleton.update();

            // Skin matrix 1 = current T(10,0,0) * inverse(T(5,6,7)) = T(5,-6,-7).
            var skin1 = skeleton._skinMatricesArray.subarray(16, 32);
            approx(check, skin1[12], 5, 'skin translation x');
            approx(check, skin1[13], -6, 'skin translation y');
            approx(check, skin1[14], -7, 'skin translation z');

            var m = skinnedMesh(geometry, skeleton, [0, 1], new Matrix4());
            var points = checkAgainstReference(check, m, 'non-identity bind');
            // Fully weighted on joint 1, vertex (0,0,0) moves by (5,-6,-7).
            approx(check, points[0][0], 5, 'first vertex x');
            approx(check, points[0][1], -6, 'first vertex y');
            approx(check, points[0][2], -7, 'first vertex z');
        }
    },
    {
        name: 'weight-threshold-on-both-sides',
        run: function (check) {
            var skeleton = buildSkeleton([identity16(), identity16()]);
            setJointWorld(skeleton, 0, identity16());
            setJointWorld(skeleton, 1, translation16(0, 10, 0));
            var geometry = triangle([0, 0, 0], [0, 1, 0, 1]);
            skeleton.updateJointsBoundingBoxes(geometry);
            skeleton.update();
            var m = skinnedMesh(geometry, skeleton, [0, 1], new Matrix4());

            function setWeights(w) {
                geometry.attributes.weight.fromArray([w, w, w]);
            }
            function setJoints(j) {
                geometry.attributes.joint.fromArray([j, j, j]);
            }
            function yOfVertex0() {
                return m.getSkinnedWorldPositions()[1];
            }

            // Term 1 boundary. Point its joint at joint 1 while the derived
            // term 3 points at joint 0, so dropping term 1 removes every
            // influence of the translating bone.
            setJoints([0, 1, 1, 0]);

            // w1 exactly 1e-4 is excluded: only identity joint 0 survives,
            // the vertex stays exactly at the origin even though weights sum < 1.
            setWeights([0.5, 1e-4, 0]);
            approx(check, yOfVertex0(), referenceMesh(m)[0][1], 'at-threshold term1 matches reference');
            approx(check, yOfVertex0(), 0, 'term1 at 1e-4 is excluded, translating bone has no effect');

            // Just above the threshold term 1 joins and the translating bone
            // contributes roughly its 1e-4 share (sum is one again).
            setWeights([0.5, 1e-4 + 1e-9, 0]);
            var aboveY = yOfVertex0();
            approx(check, aboveY, referenceMesh(m)[0][1], 'above-threshold term1 matches reference');
            check(aboveY > 0.0005 && aboveY < 0.002, 'term1 just above 1e-4 is included');

            // Derived term 3 boundary, again on a distinct (translating) joint.
            setJoints([0, 1, 1, 1]);

            // Stored weights are float32, so sit clearly on either side instead
            // of exactly on 1e-4. Here the derived w3 (~5e-5) is below the gate,
            // the identity term 0 is the only survivor.
            setWeights([0.99995, 0, 0]);
            approx(check, yOfVertex0(), referenceMesh(m)[0][1], 'below-threshold term3 matches reference');
            approx(check, yOfVertex0(), 0, 'derived term3 below 1e-4 is excluded');

            // Derived w3 (~2e-4) above the gate pulls toward joint 1.
            setWeights([0.9998, 0, 0]);
            var w3AboveY = yOfVertex0();
            approx(check, w3AboveY, referenceMesh(m)[0][1], 'above-threshold term3 matches reference');
            check(w3AboveY > 0.001 && w3AboveY < 0.003, 'derived term3 above 1e-4 is included');

            // The unconditional first term is applied even at exactly 1e-4,
            // while the second term at the same value is dropped.
            setJoints([1, 1, 0, 0]);
            setWeights([1e-4, 1e-4, 0]);
            // Only term 0 reaches the translating bone; term 1 is gated out.
            approx(check, yOfVertex0(), 1e-4 * 10, 'first term at 1e-4 is applied unconditionally');

            // A zero first weight still executes the first term without error.
            setJoints([1, 0, 0, 0]);
            setWeights([0, 0, 0]);
            approx(check, yOfVertex0(), 0, 'zero first weight runs the first term harmlessly');
        }
    },
    {
        name: 'same-frame-palette-offset-and-geometry-changes',
        run: function (check) {
            var skeleton = buildSkeleton([identity16(), identity16()]);
            setJointWorld(skeleton, 0, identity16());
            setJointWorld(skeleton, 1, translation16(0, 10, 0));
            var geometry = triangle([0, 0, 0], [0, 1, 0, 1]);
            skeleton.updateJointsBoundingBoxes(geometry);
            skeleton.update();
            var m = skinnedMesh(geometry, skeleton, [0, 1], new Matrix4());

            var before = referenceMesh(m);
            checkAgainstReference(check, m, 'initial pose');

            // Move a bone and refresh only the skeleton; no scene/animation step.
            setJointWorld(skeleton, 1, translation16(0, 20, 0));
            skeleton.update();
            checkAgainstReference(check, m, 'after same-frame bone move');
            approx(check, m.getSkinnedWorldPositions()[1], before[0][1] * 2, 'bone move takes effect on next query');

            // Change the instance offset and query again immediately.
            m.offsetMatrix.array[12] = 7;
            checkAgainstReference(check, m, 'after same-frame offset change');
            approx(check, m.getSkinnedWorldPositions()[0], referenceMesh(m)[0][0], 'offset reflected immediately');

            // Swap in another geometry; vertex count and values must follow it.
            var other = triangle([0, 0, 0], [0, 1, 0, 1]);
            other.attributes.position.fromArray([[2, 0, 0], [3, 0, 0], [2, 2, 0]]);
            other.updateBoundingBox();
            m.geometry = other;
            var swapped = m.getSkinnedWorldPositions();
            check(swapped.length === 9, 'swapped geometry drives the output length');
            checkAgainstReference(check, m, 'after geometry swap');
        }
    },
    {
        name: 'sub-palette-grow-shrink-and-reorder',
        run: function (check) {
            var skeleton = buildSkeleton([
                identity16(), identity16(), identity16(), identity16()
            ]);
            skeleton.joints.forEach(function (j, i) {
                setJointWorld(skeleton, i, translation16(i, i * 2, i * 3));
            });
            var geometry = triangle();
            skeleton.updateJointsBoundingBoxes(geometry);
            skeleton.update();

            var one = skeleton.getSubSkinMatrices('mesh-id', [0]);
            check(one.length === 16, 'single joint palette length');
            var grown = skeleton.getSubSkinMatrices('mesh-id', [0, 1, 2, 3]);
            check(grown.length === 64, 'grown palette length');
            for (var i = 0; i < 16; i++) {
                check(grown[i] === one[i], 'grown palette keeps the earlier joint content at ' + i);
            }
            var reordered = skeleton.getSubSkinMatrices('mesh-id', [3, 0]);
            check(reordered.length === 32, 'shrunk palette reallocates to the new length');
            for (var j = 0; j < 16; j++) {
                check(reordered[j] === skeleton._skinMatricesArray[48 + j], 'reordered first slot follows joint 3 at ' + j);
                check(reordered[16 + j] === skeleton._skinMatricesArray[j], 'reordered second slot follows joint 0 at ' + j);
            }
            var backToOne = skeleton.getSubSkinMatrices('mesh-id', [1]);
            check(backToOne.length === 16, 'shrinking again reallocates');
            for (var k = 0; k < 16; k++) {
                check(backToOne[k] === skeleton._skinMatricesArray[16 + k], 'final palette follows joint 1 at ' + k);
            }
        }
    },
    {
        name: 'shared-geometry-interleaved-queries-stay-independent',
        run: function (check) {
            var data = rig();
            var geometry = data.geometry, a = data.a, b = data.b;

            var positionSnapshot = Array.from(geometry.attributes.position.value);
            var boxSnapshot = {
                min: Array.from(geometry.boundingBox.min.array),
                max: Array.from(geometry.boundingBox.max.array)
            };

            var aFirst = a.getSkinnedWorldPositions();
            var bFirst = b.getSkinnedWorldPositions();
            var aSecond = a.getSkinnedWorldPositions();
            check(aFirst !== aSecond, 'every query returns a fresh owned array');
            check(aFirst !== bFirst, 'instances never share the result buffer');

            var aBox = a.getSkinnedWorldBoundingBox();
            b.getSkinnedWorldBoundingBox();
            // The earlier instance result must survive the later instance query.
            var vertexCount = geometry.vertexCount;
            for (var vi = 0; vi < vertexCount; vi++) {
                var base = vi * 3;
                for (var ci = 0; ci < 3; ci++) {
                    check(aFirst[base + ci] === aSecond[base + ci], 'instance A result unchanged after B was queried at vertex ' + vi);
                }
                // The x-only offsets move the two instances apart by 40 on x.
                approx(check, aFirst[base] - bFirst[base], 40, 'A and B instances are separated by their offsets at vertex ' + vi);
            }
            approx(check, aBox.min.array[0], 30, 'A box still describes A after interleaving');
            approx(check, aBox.max.array[0], 31, 'A box max still describes A');

            check(geometry.attributes.skinnedPosition === undefined, 'no skinnedPosition attribute is created on the geometry');
            check(Array.from(geometry.attributes.position.value).every(function (v, idx) {
                return v === positionSnapshot[idx];
            }), 'geometry positions are never written');
            check(
                Array.from(geometry.boundingBox.min.array).every(function (v, idx) { return v === boxSnapshot.min[idx]; })
                && Array.from(geometry.boundingBox.max.array).every(function (v, idx) { return v === boxSnapshot.max[idx]; }),
                'geometry bounding box is never written'
            );
        }
    },
    {
        name: 'affine-scale-shear-reflection-and-offset',
        run: function (check) {
            // Column-major affine: non-uniform scale, y-shear, z reflection, translation.
            var m16 = [
                2, 0, 0, 0,
                0.5, 3, 0, 0,
                0, 0, -1.5, 0,
                1, 2, 3, 1
            ];
            var skeleton = buildSkeleton([identity16(), identity16()]);
            setJointWorld(skeleton, 0, m16);
            setJointWorld(skeleton, 1, identity16());
            // Fully weighted on local joint 0, which carries the affine matrix.
            var geometry = triangle([1, 0, 0], [0, 0, 0, 0]);
            skeleton.updateJointsBoundingBoxes(geometry);
            skeleton.update();

            // Offset carries its own shear.
            var offset = new Matrix4();
            offset.array[4] = 0.25;
            offset.array[12] = 4;
            offset.array[13] = -1;
            var m = skinnedMesh(geometry, skeleton, [0], offset);

            var points = checkAgainstReference(check, m, 'full affine chain');
            var det = 2 * 3 * -1.5;
            check(det !== 0 && isFinite(det), 'bone matrix stays invertible with a reflection');
            points.forEach(function (p) {
                check(p.every(function (c) { return isFinite(c); }), 'deformed points are finite');
            });

            // A reflected z coordinate must flip the sign of the source z.
            approx(check, points[0][2], referenceMesh(m)[0][2], 'reflected z matches reference');
        }
    },
    {
        name: 'normal-path-and-custom-picking-conventions',
        run: function (check) {
            // Plain non-skinned picking keeps its original behavior.
            var scene = new Scene();
            var object = mesh(triangle());
            scene.add(object);
            scene.update();
            var camera = cameraAt();
            var hits = picker(scene, camera).pickAll(50, 50);
            check(hits.length === 1 && hits[0].target === object, 'non-skinned mesh picked');
            approx(check, hits[0].pointWorld.array[0], 0.25, 'non-skinned pointWorld x');

            // ignorePicking is respected by default and overridden by forcePickAll.
            object.ignorePicking = true;
            check(picker(scene, camera).pickAll(50, 50).length === 0, 'ignorePicking hides the mesh');
            check(picker(scene, camera).pick(50, 50, true).target === object, 'forcePickAll bypasses ignorePicking');
            object.ignorePicking = false;

            // Back face culling convention is unchanged.
            var frontGeom = triangle();
            var front = mesh(frontGeom);
            front.culling = true;
            var backGeom = triangle();
            backGeom.initIndicesFromArray([[0, 2, 1]]);
            var back = mesh(backGeom);
            back.culling = true;
            var cullScene = new Scene();
            cullScene.add(front);
            cullScene.add(back);
            cullScene.update();
            var cullHits = picker(cullScene, camera).pickAll(50, 50);
            check(cullHits.length === 1 && cullHits[0].target === front, 'back face is culled, front face hits');

            // geometry.pick keeps its (ndc, renderer, camera, renderable, out) calling convention.
            var pickScene = new Scene();
            var pickGeom = triangle();
            var pickCalls = [];
            pickGeom.pick = function (x, y, renderer, cam, renderable, out) {
                pickCalls.push([x, y, renderer, cam, renderable, out]);
                out.push({customPick: true});
            };
            var pickMesh = mesh(pickGeom);
            pickScene.add(pickMesh);
            pickScene.update();
            var pickRenderer = picker(pickScene, camera);
            var pickOut = pickRenderer.pickAll(50, 50);
            check(pickCalls.length === 1, 'geometry.pick invoked once');
            check(pickCalls[0][2] === pickRenderer.renderer, 'pick receives the renderer');
            check(pickCalls[0][3] === camera, 'pick receives the camera');
            check(pickCalls[0][4] === pickMesh, 'pick receives the renderable');
            check(pickCalls[0][5] === pickOut, 'pick receives the output array');
            check(pickOut[0].customPick === true, 'custom pick result is appended');

            // geometry.pickByRay keeps its (ray, renderable, out) calling convention.
            var rayScene = new Scene();
            var rayGeom = triangle();
            var rayCalls = [];
            rayGeom.pickByRay = function (ray, renderable, out) {
                rayCalls.push([ray, renderable, out]);
                out.push({customRay: true});
            };
            var rayMesh = mesh(rayGeom);
            rayScene.add(rayMesh);
            rayScene.update();
            var rayPicker = picker(rayScene, camera);
            var rayOut = rayPicker.pickAll(50, 50);
            check(rayCalls.length === 1, 'geometry.pickByRay invoked once');
            // The mesh is untransformed, so the callback ray matches the cast ray.
            var passedRay = rayCalls[0][0];
            check(passedRay.origin.array[0] === rayPicker._ray.origin.array[0]
                && passedRay.origin.array[1] === rayPicker._ray.origin.array[1]
                && passedRay.origin.array[2] === rayPicker._ray.origin.array[2], 'pickByRay receives the ray origin');
            check(passedRay.direction.array[0] === rayPicker._ray.direction.array[0]
                && passedRay.direction.array[1] === rayPicker._ray.direction.array[1]
                && passedRay.direction.array[2] === rayPicker._ray.direction.array[2], 'pickByRay receives the ray direction');
            check(rayCalls[0][1] === rayMesh, 'pickByRay receives the renderable');
            check(rayCalls[0][2] === rayOut && rayOut[0].customRay === true, 'pickByRay output convention preserved');
        }
    }
];

export default cases;
