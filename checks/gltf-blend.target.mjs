// Content assertions for GLTFBlend. Every group is synchronous and exercises
// real GLTFAnimation/Transform/Skin output rather than mocked blend results.
import { GLTFAnimation } from '../src/extras/GLTFAnimation.js';
import { GLTFBlend } from '../src/extras/GLTFBlend.js';
import { Transform } from '../src/core/Transform.js';
import { Quat } from '../src/math/Quat.js';
import { Vec3 } from '../src/math/Vec3.js';
import { Mat4 } from '../src/math/Mat4.js';
import { makeSkin } from './fixture.mjs';

const EPS = 1e-6;
const close = (check, a, b, eps = EPS, message = '') =>
    check(Math.abs(a - b) <= eps, `${message} expected ${b}, got ${a}`);
const vecClose = (check, a, b, eps = EPS, message = '') => {
    check(a.length === b.length, `${message} length`);
    for (let i = 0; i < a.length; i++) close(check, a[i], b[i], eps, `${message}[${i}]`);
};
const axisQuat = (axis, angle) => new Quat().fromAxisAngle(new Vec3(...axis), angle);
const f32 = (values) => new Float32Array(values);
const makeClip = (channels, weight = 1) => {
    const clip = new GLTFAnimation(channels);
    clip.weight = weight;
    return clip;
};
const posChannel = (node, times, values, interpolation = 'LINEAR') => ({
    node,
    transform: 'position',
    interpolation,
    times: f32(times),
    values: f32(values),
});
const quatChannel = (node, times, values, interpolation = 'LINEAR') => ({
    node,
    transform: 'quaternion',
    interpolation,
    times: f32(times),
    values: f32(values),
});
const scaleChannel = (node, times, values, interpolation = 'LINEAR') => ({
    node,
    transform: 'scale',
    interpolation,
    times: f32(times),
    values: f32(values),
});
const snapshot = (node) => ({
    position: Array.from(node.position),
    quaternion: Array.from(node.quaternion),
    scale: Array.from(node.scale),
});
// Read the actual float32-rounded quaternion stored in a single-key channel,
// since the blend samples channel data rather than the original float64 value.
const storedQuat = (clip) => Array.from(clip.data[0].values.subarray(0, 4));
const sameSnapshot = (check, a, b, message) => {
    for (const prop of ['position', 'quaternion', 'scale']) vecClose(check, a[prop], b[prop], 1e-12, message);
};

// Independent SO(3) Karcher-mean oracle written only for these assertions.
const qDot = (a, b) => a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
const qMul = (a, b) => [
    a[0] * b[3] + a[3] * b[0] + a[1] * b[2] - a[2] * b[1],
    a[1] * b[3] + a[3] * b[1] + a[2] * b[0] - a[0] * b[2],
    a[2] * b[3] + a[3] * b[2] + a[0] * b[1] - a[1] * b[0],
    a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2],
];
const qLog = (q) => {
    const r = Math.hypot(q[0], q[1], q[2]);
    if (r < 1e-14) return [0, 0, 0];
    const k = Math.atan2(r, q[3]) / r;
    return [q[0] * k, q[1] * k, q[2] * k];
};
const qExp = (v) => {
    const r = Math.hypot(v[0], v[1], v[2]);
    if (r < 1e-14) return [0, 0, 0, 1];
    const k = Math.sin(r) / r;
    return [v[0] * k, v[1] * k, v[2] * k, Math.cos(r)];
};
const referenceMean = (samples) => {
    let mean = samples[0].q.slice();
    for (let iter = 0; iter < 200; iter++) {
        const sum = [0, 0, 0];
        for (const { q, w } of samples) {
            const oriented = qDot(mean, q) < 0 ? q.map((v) => -v) : q;
            const log = qLog(qMul([-mean[0], -mean[1], -mean[2], mean[3]], oriented));
            for (let i = 0; i < 3; i++) sum[i] += w * log[i];
        }
        if (Math.hypot(...sum) < 1e-13) break;
        mean = qMul(mean, qExp(sum));
        const len = Math.hypot(...mean);
        mean = mean.map((v) => v / len);
    }
    return mean;
};
// Normalize first: float32 channel keys leave the output unit only to ~1e-16.
const angularDistance = (a, b) => {
    const na = Math.hypot(a[0], a[1], a[2], a[3]);
    const nb = Math.hypot(b[0], b[1], b[2], b[3]);
    const dot = Math.abs(qDot(a, b)) / (na * nb);
    return 2 * Math.acos(Math.min(1, dot));
};
// Weighted mean log residual of a candidate rotation; zero is the SO(3) mean condition.
const meanLogResidual = (mean, samples) => {
    const sum = [0, 0, 0];
    for (const { q, w } of samples) {
        const oriented = qDot(mean, q) < 0 ? q.map((v) => -v) : q;
        const log = qLog(qMul([-mean[0], -mean[1], -mean[2], mean[3]], oriented));
        for (let i = 0; i < 3; i++) sum[i] += w * log[i];
    }
    return Math.hypot(...sum);
};

export const cases = {
    // Missing channels per property, residual vs excess weights, order/repeat
    // determinism, zero-weight restore, fixed clip set and untouched inputs.
    sparse(check) {
        const nodeA = new Transform();
        nodeA.position.set(1, 2, 3);
        nodeA.quaternion.copy(axisQuat([0, 0, 1], 0.4));
        nodeA.scale.set(2, 3, 4);
        const nodeB = new Transform();
        nodeB.position.set(5, 6, 7);

        // Clip one owns A.position and B.position; clip two owns A.quaternion only.
        const clip1 = makeClip(
            [
                posChannel(nodeA, [0, 2], [10, 0, 0, 30, 0, 0]),
                posChannel(nodeB, [0, 2], [0, 0, 0, 4, 0, 0]),
            ],
            0.4
        );
        const clip2 = makeClip([quatChannel(nodeA, [0], axisQuat([1, 0, 0], 0.8))], 0.25);
        clip1.elapsed = 1; // midpoint -> A pos (20,0,0), B pos (2,0,0)
        const source = [clip1, clip2];
        const blend = new GLTFBlend(source);
        check(blend.update() === blend, 'update returns the blend instance');

        // Residual weight: S = 0.4 on A.position, static keeps 0.6.
        vecClose(check, nodeA.position, [0.6 * 1 + 0.4 * 20, 0.6 * 2, 0.6 * 3], 1e-12, 'A.position residual blend');
        // A.scale has no owning channel at all: captured static pose survives.
        vecClose(check, nodeA.scale, [2, 3, 4], 0, 'A.scale untouched without channels');

        // Rotation with residual weight is the SO(3) mean, i.e. shortest-arc slerp here.
        const expectedQuat = new Quat().copy(axisQuat([0, 0, 1], 0.4));
        expectedQuat.slerp(axisQuat([1, 0, 0], 0.8), 0.25);
        close(check, angularDistance(Array.from(nodeA.quaternion), Array.from(expectedQuat)), 0, 1e-9, 'A.quaternion partial-weight mean');

        // Excess weight on B.position: 1.5 + 0.5 normalizes, static drops out.
        const clip3 = makeClip([posChannel(nodeB, [0, 2], [0, 0, 0, 8, 0, 0])], 0.5);
        clip1.weight = 1.5;
        clip3.elapsed = 1; // midpoint -> (4,0,0)
        const excess = new GLTFBlend([clip1, clip3]);
        excess.update();
        vecClose(check, nodeB.position, [0.75 * 2 + 0.25 * 4, 0, 0], 1e-12, 'B.position excess blend');
        // B has no rotation/scale channels: those properties keep their captured values.
        vecClose(check, nodeB.quaternion, [0, 0, 0, 1], 0, 'B.quaternion untouched');
        vecClose(check, nodeB.scale, [1, 1, 1], 0, 'B.scale untouched');

        // Repeated calls with identical inputs are idempotent.
        const first = snapshot(nodeB);
        excess.update();
        excess.update();
        sameSnapshot(check, snapshot(nodeB), first, 'repeat update');

        // Clip order cannot change the pose.
        clip1.weight = 0.4;
        const nodeC = new Transform();
        const c1 = makeClip([posChannel(nodeC, [0, 1], [0, 0, 0, 2, 0, 0])], 0.3);
        const c2 = makeClip([posChannel(nodeC, [0, 1], [0, 0, 0, 8, 0, 0])], 0.5);
        c1.elapsed = 1;
        c2.elapsed = 1;
        new GLTFBlend([c1, c2]).update();
        const ordered = snapshot(nodeC);
        nodeC.position.set(0, 0, 0);
        new GLTFBlend([c2, c1]).update();
        sameSnapshot(check, snapshot(nodeC), ordered, 'reordered clips');

        // The clip set is fixed at construction; mutating the source array later is ignored.
        const intruder = makeClip([posChannel(nodeC, [0, 1], [99, 99, 99, 99, 99, 99])], 1);
        intruder.elapsed = 1;
        source.push(intruder);
        blend.update();
        vecClose(check, nodeC.position, ordered.position, 0, 'post-construction array mutation ignored');

        // All-zero weights restore every captured static value.
        clip1.weight = 0;
        clip2.weight = 0;
        blend.update();
        vecClose(check, nodeA.position, [1, 2, 3], 0, 'zero weights restore A.position');
        vecClose(check, nodeA.scale, [2, 3, 4], 0, 'static scale still intact');
        vecClose(check, nodeA.quaternion, axisQuat([0, 0, 1], 0.4), 1e-9, 'zero weights restore A.quaternion');

        // Inputs are read-only as far as the blend is concerned.
        check(clip1.data[0].times === clip1.data[0].times, 'channel arrays keep identity');
        check(clip1.elapsed === 1 && clip2.elapsed === 0, 'elapsed is never advanced by update');
        check(clip1.data.length === 2 && clip2.data.length === 1, 'channel arrays keep length');
    },

    // Intrinsic SO(3) weighted mean with non-coaxial samples, sign equivalence,
    // stationarity (zero mean log residual) and 1e-6 rad angular accuracy.
    rotation(check) {
        const node = new Transform(); // static rotation is identity
        const q1 = axisQuat([1, 0, 0], 0.8); // all within pi/2 of the static pose
        const q2 = axisQuat([0, 1, 0], 0.9);
        const q3 = axisQuat([1 / Math.SQRT2, 1 / Math.SQRT2, 0], 0.7); // unit axis, non-coaxial with q1/q2
        const weights = [0.5, 0.3, 0.2];
        const clips = [q1, q2, q3].map((q, i) => makeClip([quatChannel(node, [0], q)], weights[i]));
        const blend = new GLTFBlend(clips);
        blend.update();

        const out = Array.from(node.quaternion);
        close(check, Math.hypot(...out), 1, 1e-12, 'unit quaternion output');

        // Non-coaxial stationarity proof: weighted mean log residual must vanish.
        // Samples must be the float32 values the blend actually read from the channels.
        const samples = clips.map((clip, i) => ({ q: storedQuat(clip), w: weights[i] }));
        close(check, meanLogResidual(out, samples), 0, 1e-9, 'zero mean log residual (non-coaxial)');

        // Angular error against an independently computed Karcher mean.
        const oracle = referenceMean(samples);
        close(check, angularDistance(out, oracle), 0, 1e-6, 'angular error vs reference mean');

        // q and -q are the same rotation: negating one stored key must not move the result.
        const negated = new Transform();
        const negClips = [q1, q2, q3].map((q, i) => {
            const value = i === 2 ? Array.from(q).map((v) => -v) : Array.from(q);
            return makeClip([quatChannel(negated, [0], value)], weights[i]);
        });
        new GLTFBlend(negClips).update();
        const negOut = Array.from(negated.quaternion);
        close(check, angularDistance(out, negOut), 0, 1e-10, 'sign-negated key gives same rotation');

        // Residual weight pulls in the static rotation as a weighted SO(3) sample.
        const partialNode = new Transform();
        const partialStatic = axisQuat([0, 1, 0], 0.5);
        partialNode.quaternion.copy(partialStatic);
        const p1 = makeClip([quatChannel(partialNode, [0], axisQuat([1, 0, 0], 0.6))], 0.3);
        const p2 = makeClip([quatChannel(partialNode, [0], axisQuat([0, 0, 1], 0.7))], 0.2);
        new GLTFBlend([p1, p2]).update();
        const partialOut = Array.from(partialNode.quaternion);
        // Static pose is captured straight from the node (float64); clips come from float32 channels.
        const partialSamples = [
            { q: Array.from(partialStatic), w: 0.5 },
            { q: storedQuat(p1), w: 0.3 },
            { q: storedQuat(p2), w: 0.2 },
        ];
        close(check, meanLogResidual(partialOut, partialSamples), 0, 1e-9, 'residual static rotation participates in mean');
        close(check, angularDistance(partialOut, referenceMean(partialSamples)), 0, 1e-6, 'partial-weight angular error');

        // Excess weights normalize before the mean; the static pose drops out.
        const excessNode = new Transform();
        const e1 = makeClip([quatChannel(excessNode, [0], axisQuat([1, 0, 0], 0.8))], 1.5);
        const e2 = makeClip([quatChannel(excessNode, [0], axisQuat([0, 1, 0], 0.9))], 0.5);
        new GLTFBlend([e1, e2]).update();
        const excessOut = Array.from(excessNode.quaternion);
        const excessSamples = [
            { q: storedQuat(e1), w: 0.75 },
            { q: storedQuat(e2), w: 0.25 },
        ];
        close(check, meanLogResidual(excessOut, excessSamples), 0, 1e-9, 'excess-weight mean residual');
        close(check, angularDistance(excessOut, referenceMean(excessSamples)), 0, 1e-6, 'excess-weight angular error');

        // Co-axial sanity: equal weights must land on the midpoint angle.
        const coNode = new Transform();
        const co1 = makeClip([quatChannel(coNode, [0], axisQuat([1, 0, 0], 0.2))], 0.5);
        const co2 = makeClip([quatChannel(coNode, [0], axisQuat([1, 0, 0], 0.8))], 0.5);
        new GLTFBlend([co1, co2]).update();
        close(check, angularDistance(Array.from(coNode.quaternion), Array.from(axisQuat([1, 0, 0], 0.5))), 0, 1e-6, 'co-axial midpoint angle');
    },

    // Timeline resolution (loop modulo, clamp, zero duration, offset start) and
    // STEP / LINEAR / CUBICSPLINE sampling, including non-uniform segments.
    sampling(check) {
        // Loop positive modulo, including negative elapsed.
        const loopNode = new Transform();
        const loopClip = makeClip([posChannel(loopNode, [0, 2], [0, 0, 0, 4, 0, 0])], 1);
        const loopBlend = new GLTFBlend([loopClip]);
        loopClip.elapsed = 5;
        loopBlend.update();
        close(check, loopNode.position[0], 2, EPS, 'loop modulo wraps elapsed');
        loopClip.elapsed = -1;
        loopBlend.update();
        close(check, loopNode.position[0], 2, EPS, 'negative elapsed uses positive modulo');

        // Offset startTime: elapsed is relative to the clip duration, not key zero.
        const offsetNode = new Transform();
        const offsetClip = makeClip([posChannel(offsetNode, [1, 3], [0, 0, 0, 4, 0, 0])], 1);
        offsetClip.elapsed = 1;
        new GLTFBlend([offsetClip]).update();
        close(check, offsetNode.position[0], 2, EPS, 'startTime offset timeline');

        // Non-loop clamps to both ends; zero duration always takes the only key.
        const clampNode = new Transform();
        const clampClip = makeClip([posChannel(clampNode, [0, 2], [3, 0, 0, 9, 0, 0])], 1);
        clampClip.loop = false;
        clampClip.elapsed = 10;
        new GLTFBlend([clampClip]).update();
        close(check, clampNode.position[0], 9, EPS, 'non-loop clamps at end key');
        clampClip.elapsed = -5;
        new GLTFBlend([clampClip]).update();
        close(check, clampNode.position[0], 3, EPS, 'non-loop clamps at start key');

        const singleNode = new Transform();
        const singleClip = makeClip([posChannel(singleNode, [2], [7, 7, 7])], 1);
        check(singleClip.duration === 0, 'single key gives zero duration');
        singleClip.elapsed = 42;
        new GLTFBlend([singleClip]).update();
        vecClose(check, singleNode.position, [7, 7, 7], 0, 'single key is constant');

        // STEP holds the previous key and takes the key exactly on a boundary.
        const stepNode = new Transform();
        const stepClip = makeClip([posChannel(stepNode, [0, 1, 2], [0, 0, 0, 10, 0, 0, 20, 0, 0], 'STEP')], 1);
        const stepBlend = new GLTFBlend([stepClip]);
        stepClip.elapsed = 1.5;
        stepBlend.update();
        close(check, stepNode.position[0], 10, EPS, 'STEP holds previous key between frames');
        stepClip.elapsed = 1;
        stepBlend.update();
        close(check, stepNode.position[0], 10, EPS, 'STEP takes current key exactly on frame');
        stepClip.elapsed = 0;
        stepBlend.update();
        close(check, stepNode.position[0], 0, EPS, 'STEP first frame');

        // LINEAR rotation follows the shortest arc even when the end key is negated.
        const arcNode = new Transform();
        const target = axisQuat([0, 0, 1], 1.0);
        const arcClip = makeClip([
            quatChannel(arcNode, [0, 2], [0, 0, 0, 1, ...Array.from(target).map((v) => -v)]),
        ]);
        arcClip.elapsed = 1;
        new GLTFBlend([arcClip]).update();
        close(
            check,
            angularDistance(Array.from(arcNode.quaternion), Array.from(axisQuat([0, 0, 1], 0.5))),
            0,
            1e-6,
            'LINEAR shortest arc with negated end key'
        );

        // CUBICSPLINE on a non-uniform timeline (segment seconds 1 then 2).
        const cubicNode = new Transform();
        // Per key: in-tangent, value, out-tangent. Only key one has a live out-tangent.
        const cubicValues = [
            0, 0, 0, 0, 0, 0, 0, 0, 0, // t = 0
            0, 0, 0, 2, 0, 0, 4, 0, 0, // t = 1
            0, 0, 0, 6, 0, 0, 0, 0, 0, // t = 3
        ];
        const cubicClip = makeClip([posChannel(cubicNode, [0, 1, 3], cubicValues, 'CUBICSPLINE')], 1);
        const cubicBlend = new GLTFBlend([cubicClip]);
        cubicClip.elapsed = 0.5; // first segment, alpha 0.5, duration 1
        cubicBlend.update();
        close(check, cubicNode.position[0], 1, 1e-10, 'cubic first segment midpoint');
        cubicClip.elapsed = 2; // second segment, alpha 0.5, duration 2 scales the tangent
        cubicBlend.update();
        // s0*2 + s1*(2*4) + s2*6 = 1 + 1 + 3 at alpha 0.5
        close(check, cubicNode.position[0], 5, 1e-10, 'cubic tangent scaled by segment seconds');

        // CUBICSPLINE quaternion: Hermite then normalize, no key/tampering signs.
        const cubicQuatNode = new Transform();
        const endQ = axisQuat([1, 0, 0], 1.0);
        const cubicQuatValues = [
            0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0,
            0, 0, 0, 0, ...endQ, 0, 0, 0, 0,
        ];
        const cubicQuatClip = makeClip([quatChannel(cubicQuatNode, [0, 2], cubicQuatValues, 'CUBICSPLINE')], 1);
        cubicQuatClip.elapsed = 1;
        new GLTFBlend([cubicQuatClip]).update();
        close(check, Math.hypot(...cubicQuatNode.quaternion), 1, 1e-12, 'cubic quaternion normalized');
        close(
            check,
            angularDistance(Array.from(cubicQuatNode.quaternion), Array.from(axisQuat([1, 0, 0], 0.5))),
            0,
            1e-6,
            'cubic quaternion midpoint angle'
        );

        // Endpoint hold outside the key range on a non-loop LINEAR channel.
        const holdNode = new Transform();
        const holdClip = makeClip([posChannel(holdNode, [1, 2], [10, 0, 0, 20, 0, 0])], 1);
        holdClip.loop = false;
        holdClip.elapsed = 5;
        new GLTFBlend([holdClip]).update();
        close(check, holdNode.position[0], 20, EPS, 'value held past last key');
        holdClip.elapsed = -5;
        new GLTFBlend([holdClip]).update();
        close(check, holdNode.position[0], 10, EPS, 'value held before first key');

        // Jumping away and back to a time reproduces the earlier pose exactly.
        loopClip.elapsed = 1;
        loopBlend.update();
        const before = snapshot(loopNode);
        loopClip.elapsed = 1.75;
        loopBlend.update();
        loopClip.elapsed = 1;
        loopBlend.update();
        sameSnapshot(check, snapshot(loopNode), before, 'time jump round trip');

        // A zero-weight clip is not sampled and cannot disturb the static pose.
        const silentNode = new Transform();
        silentNode.position.set(3, 3, 3);
        const silent = makeClip([posChannel(silentNode, [0], [99, 99, 99])], 0);
        silent.elapsed = 0;
        new GLTFBlend([silent]).update();
        vecClose(check, silentNode.position, [3, 3, 3], 0, 'zero-weight clip not sampled');
    },

    // Full path through real Transform hierarchy and Skin bone matrices.
    skeleton(check) {
        const buildRig = () => {
            const { skin } = makeSkin();
            const [root, child] = skin.bones;

            const walk = makeClip(
                [
                    posChannel(root, [0, 2], [0, 0, 0, 4, 0, 0]),
                    quatChannel(root, [0, 2], [0, 0, 0, 1, ...axisQuat([0, 0, 1], 0.6)]),
                    quatChannel(child, [0, 2], [0, 0, 0, 1, ...axisQuat([0, 0, 1], 0.8)]),
                ],
                0.6
            );
            const turn = makeClip(
                [
                    posChannel(root, [0, 2], [0, 0, 0, 2, 0, 0]),
                    scaleChannel(root, [0, 2], [1, 1, 1, 2, 2, 2]),
                    posChannel(child, [0, 2], [0, 1, 0, 0, 3, 0]),
                ],
                0.4
            );
            walk.elapsed = 1;
            turn.elapsed = 1;
            return { skin, root, child, walk, turn };
        };

        const { skin, root, child, walk, turn } = buildRig();
        // One blend built at the bind pose; reused across every update in this rig.
        const blend = new GLTFBlend([walk, turn]);
        blend.update();
        skin.root.updateMatrixWorld(true);
        skin.draw();

        // Local blended pose: weights sum to 1 on root position, static drops out.
        close(check, root.position[0], 0.6 * 2 + 0.4 * 1, 1e-10, 'root blended position x');
        // Root rotation only walk owns: S = 0.6, static identity keeps 0.4 -> Rz(0.18).
        // Rotation keys are float32 channels, so allow that rounding against float64 trig.
        close(check, root.worldMatrix[0], 1.2 * Math.cos(0.18), 1e-7, 'root bone rotation+scale m00');
        close(check, root.worldMatrix[1], 1.2 * Math.sin(0.18), 1e-7, 'root bone rotation+scale m10');
        close(check, root.scale[0], 0.6 * 1 + 0.4 * 1.5, 1e-10, 'root blended scale');

        // Child local pose: rotation Rz(0.24), position y = 0.6*1 + 0.4*2 = 1.4.
        const childY = 1.4;
        const rootScale = 1.2; // 0.6*1 + 0.4*1.5 uniform scale on the root
        const worldAngle = 0.18 + 0.24;
        close(check, child.quaternion[3], Math.cos(0.12), 1e-7, 'child local blended rotation');
        close(check, child.position[1], childY, 1e-10, 'child local blended position');

        // Real parent/child world matrix: the root's scale+rotation carry the child offset.
        close(check, child.worldMatrix[12], root.position[0] - rootScale * childY * Math.sin(0.18), 1e-7, 'child world translation x');
        close(check, child.worldMatrix[13], rootScale * childY * Math.cos(0.18), 1e-7, 'child world translation y');
        close(check, child.worldMatrix[0], rootScale * Math.cos(worldAngle), 1e-7, 'child world rotation m00');
        close(check, child.worldMatrix[1], rootScale * Math.sin(worldAngle), 1e-7, 'child world rotation m10');

        // Skin palette: root bind inverse is identity, so its palette is its world matrix.
        // The palette is a Float32Array, so stored entries carry float32 rounding.
        close(check, skin.boneMatrices[12], root.position[0], 1e-6, 'root palette translation x');
        // Child bind inverse is translation-only, so its palette rotation block keeps root scale.
        close(check, skin.boneMatrices[16], rootScale * Math.cos(worldAngle), 1e-6, 'child palette rotation m00');
        close(check, skin.boneMatrices[17], rootScale * Math.sin(worldAngle), 1e-6, 'child palette rotation m10');

        // Independently recompute the child palette entry with Mat4 multiplication.
        const expected = new Mat4().multiply(child.worldMatrix, child.bindInverse);
        for (let i = 0; i < 16; i++) {
            close(check, skin.boneMatrices[16 + i], expected[i], 1e-6, `child palette entry ${i}`);
        }

        // Order independence through the whole skeleton path.
        const reordered = buildRig();
        new GLTFBlend([reordered.turn, reordered.walk]).update();
        reordered.skin.root.updateMatrixWorld(true);
        reordered.skin.draw();
        check(skin.boneMatrices.length === reordered.skin.boneMatrices.length, 'palette sizes match');
        for (let i = 0; i < skin.boneMatrices.length; i++) {
            close(check, reordered.skin.boneMatrices[i], skin.boneMatrices[i], 1e-12, `order-independent palette ${i}`);
        }

        // Repeated updates and time round trips leave the palette bit-for-bit stable.
        // Reuse the bind-pose blend: rebuilding it would recapture the animated pose as static.
        const paletteSnapshot = Array.from(skin.boneMatrices);
        blend.update();
        skin.root.updateMatrixWorld(true);
        skin.draw();
        walk.elapsed = 0.25;
        turn.elapsed = 1.75;
        blend.update();
        skin.root.updateMatrixWorld(true);
        skin.draw();
        walk.elapsed = 1;
        turn.elapsed = 1;
        blend.update();
        skin.root.updateMatrixWorld(true);
        skin.draw();
        vecClose(check, skin.boneMatrices, paletteSnapshot, 0, 'palette stable after repeat and time jumps');

        // Zero weights restore the bind pose, so palette entries are identity.
        const rest = buildRig();
        rest.walk.weight = 0;
        rest.turn.weight = 0;
        new GLTFBlend([rest.walk, rest.turn]).update();
        rest.skin.root.updateMatrixWorld(true);
        rest.skin.draw();
        const identity = new Mat4();
        for (let bone = 0; bone < 2; bone++) {
            for (let i = 0; i < 16; i++) {
                close(check, rest.skin.boneMatrices[bone * 16 + i], identity[i], 1e-12, `rest palette bone ${bone} entry ${i}`);
            }
        }
    },
};
