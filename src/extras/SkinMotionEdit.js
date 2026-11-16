import { GLTFAnimation } from './GLTFAnimation.js';

/*
 * Skin motion editing solver.
 *
 * All math here operates on plain Arrays / Float64Arrays so that solving never
 * touches the caller's Transform nodes, world matrices, textures or animation
 * channels. The only scene-graph access is read-only, and the shadow palette
 * uses the same worldMatrix * bindInverse assembly as GLTFSkin.updateUniforms.
 */

const isFiniteNumber = (v) => typeof v === 'number' && Number.isFinite(v);
const isPositiveFinite = (v) => isFiniteNumber(v) && v > 0;

function fail(message) {
    throw new Error(`editSkinMotion: ${message}`);
}

// ---------------------------------------------------------------------------
// Quaternion / matrix helpers (column-major mat4, gl-matrix conventions)
// ---------------------------------------------------------------------------

function qMultiply(out, a, b) {
    const ax = a[0],
        ay = a[1],
        az = a[2],
        aw = a[3];
    const bx = b[0],
        by = b[1],
        bz = b[2],
        bw = b[3];
    out[0] = ax * bw + aw * bx + ay * bz - az * by;
    out[1] = ay * bw + aw * by + az * bx - ax * bz;
    out[2] = az * bw + aw * bz + ax * by - ay * bx;
    out[3] = aw * bw - ax * bx - ay * by - az * bz;
    return out;
}

function qNormalize(out, a) {
    const len = Math.hypot(a[0], a[1], a[2], a[3]) || 1;
    out[0] = a[0] / len;
    out[1] = a[1] / len;
    out[2] = a[2] / len;
    out[3] = a[3] / len;
    return out;
}

// Rodrigues exponential map: Exp(v) is the unit quaternion rotating by |v|
// around the axis v/|v|. Its differential at v = 0 is the identity on twist
// vectors (d Exp_0(dv) rotates a point p by dv x p), which is the
// skew-symmetric Jacobian used throughout the analytic gradient.
function qExp(out, v) {
    const x = v[0];
    const y = v[1];
    const z = v[2];
    const theta2 = x * x + y * y + z * z;
    const theta = Math.sqrt(theta2);
    if (theta < 1e-8) {
        // Taylor expansions keep the small-angle path smooth.
        const s = 0.5 - theta2 / 48;
        out[0] = x * s;
        out[1] = y * s;
        out[2] = z * s;
        out[3] = 1 - theta2 / 8;
        return out;
    }
    const half = theta * 0.5;
    const s = Math.sin(half) / theta;
    out[0] = x * s;
    out[1] = y * s;
    out[2] = z * s;
    out[3] = Math.cos(half);
    return out;
}

// Shortest-arc slerp between unit quaternions, matching GLTFAnimation playback.
function qSlerp(out, a, b, t) {
    let bx = b[0],
        by = b[1],
        bz = b[2],
        bw = b[3];
    const cosom = a[0] * bx + a[1] * by + a[2] * bz + a[3] * bw;
    if (cosom < 0) {
        bx = -bx;
        by = -by;
        bz = -bz;
        bw = -bw;
    }
    const cos = Math.abs(cosom);
    let s0, s1;
    if (1 - cos > 1e-10) {
        const omega = Math.acos(cos);
        const sinom = Math.sin(omega);
        s0 = Math.sin((1 - t) * omega) / sinom;
        s1 = Math.sin(t * omega) / sinom;
    } else {
        s0 = 1 - t;
        s1 = t;
    }
    out[0] = s0 * a[0] + s1 * bx;
    out[1] = s0 * a[1] + s1 * by;
    out[2] = s0 * a[2] + s1 * bz;
    out[3] = s0 * a[3] + s1 * bw;
    return qNormalize(out, out);
}

function m4FromQuat(out, q) {
    const x = q[0],
        y = q[1],
        z = q[2],
        w = q[3];
    const x2 = x + x,
        y2 = y + y,
        z2 = z + z;
    const xx = x * x2,
        xy = x * y2,
        xz = x * z2;
    const yy = y * y2,
        yz = y * z2,
        zz = z * z2;
    const wx = w * x2,
        wy = w * y2,
        wz = w * z2;
    out[0] = 1 - (yy + zz);
    out[1] = xy + wz;
    out[2] = xz - wy;
    out[3] = 0;
    out[4] = xy - wz;
    out[5] = 1 - (xx + zz);
    out[6] = yz + wx;
    out[7] = 0;
    out[8] = xz + wy;
    out[9] = yz - wx;
    out[10] = 1 - (xx + yy);
    out[11] = 0;
    out[15] = 1;
    return out;
}

// Local scale is guaranteed to be 1, so a local matrix is T * R only.
function m4ComposeTR(out, pos, q) {
    m4FromQuat(out, q);
    out[12] = pos[0];
    out[13] = pos[1];
    out[14] = pos[2];
    return out;
}

function m4Multiply(out, a, b) {
    const a00 = a[0],
        a01 = a[1],
        a02 = a[2],
        a03 = a[3];
    const a10 = a[4],
        a11 = a[5],
        a12 = a[6],
        a13 = a[7];
    const a20 = a[8],
        a21 = a[9],
        a22 = a[10],
        a23 = a[11];
    const a30 = a[12],
        a31 = a[13],
        a32 = a[14],
        a33 = a[15];
    let b0 = b[0],
        b1 = b[1],
        b2 = b[2],
        b3 = b[3];
    out[0] = b0 * a00 + b1 * a10 + b2 * a20 + b3 * a30;
    out[1] = b0 * a01 + b1 * a11 + b2 * a21 + b3 * a31;
    out[2] = b0 * a02 + b1 * a12 + b2 * a22 + b3 * a32;
    out[3] = b0 * a03 + b1 * a13 + b2 * a23 + b3 * a33;
    b0 = b[4];
    b1 = b[5];
    b2 = b[6];
    b3 = b[7];
    out[4] = b0 * a00 + b1 * a10 + b2 * a20 + b3 * a30;
    out[5] = b0 * a01 + b1 * a11 + b2 * a21 + b3 * a31;
    out[6] = b0 * a02 + b1 * a12 + b2 * a22 + b3 * a32;
    out[7] = b0 * a03 + b1 * a13 + b2 * a23 + b3 * a33;
    b0 = b[8];
    b1 = b[9];
    b2 = b[10];
    b3 = b[11];
    out[8] = b0 * a00 + b1 * a10 + b2 * a20 + b3 * a30;
    out[9] = b0 * a01 + b1 * a11 + b2 * a21 + b3 * a31;
    out[10] = b0 * a02 + b1 * a12 + b2 * a22 + b3 * a32;
    out[11] = b0 * a03 + b1 * a13 + b2 * a23 + b3 * a33;
    b0 = b[12];
    b1 = b[13];
    b2 = b[14];
    b3 = b[15];
    out[12] = b0 * a00 + b1 * a10 + b2 * a20 + b3 * a30;
    out[13] = b0 * a01 + b1 * a11 + b2 * a21 + b3 * a31;
    out[14] = b0 * a02 + b1 * a12 + b2 * a22 + b3 * a32;
    out[15] = b0 * a03 + b1 * a13 + b2 * a23 + b3 * a33;
    return out;
}

function m4TransformPoint(m, p, out) {
    const x = p[0],
        y = p[1],
        z = p[2];
    out[0] = m[0] * x + m[4] * y + m[8] * z + m[12];
    out[1] = m[1] * x + m[5] * y + m[9] * z + m[13];
    out[2] = m[2] * x + m[6] * y + m[10] * z + m[14];
    return out;
}

// Analytic Jacobian of f(x) = Exp(x) v, columns are df/dx_i. Built from the
// Rodrigues form R(x) = I + s [x]_x + c2 [x]_x^2, so it contains the full
// exponential-map derivative (at x = 0 it reduces to -[v]_x). Row-major 3x3.
function expPointJacobian(x, v) {
    const theta2 = x[0] * x[0] + x[1] * x[1] + x[2] * x[2];
    const theta = Math.sqrt(theta2);
    const u = [x[1] * v[2] - x[2] * v[1], x[2] * v[0] - x[0] * v[2], x[0] * v[1] - x[1] * v[0]];
    const w = [x[1] * u[2] - x[2] * u[1], x[2] * u[0] - x[0] * u[2], x[0] * u[1] - x[1] * u[0]];
    let s, sp, c2, c2p;
    if (theta < 1e-6) {
        // Taylor expansions of sin(th)/th and (1-cos(th))/th^2, and of their
        // derivatives with respect to theta (the per-component derivative then
        // multiplies these by d(theta)/d(x_i) = x_i/theta).
        s = 1 - theta2 / 6 + theta2 * theta2 / 120;
        sp = -theta / 3 + (theta * theta2) / 30;
        c2 = 0.5 - theta2 / 24 + theta2 * theta2 / 720;
        c2p = -theta / 12 + (theta * theta2) / 180;
    } else {
        s = Math.sin(theta) / theta;
        sp = (theta * Math.cos(theta) - Math.sin(theta)) / theta2;
        c2 = (1 - Math.cos(theta)) / theta2;
        c2p = (theta * Math.sin(theta) - 2 * (1 - Math.cos(theta))) / (theta2 * theta);
    }
    const out = new Float64Array(9);
    for (let i = 0; i < 3; i++) {
        const ei = [i === 0 ? 1 : 0, i === 1 ? 1 : 0, i === 2 ? 1 : 0];
        const eiCrossV = [ei[1] * v[2] - ei[2] * v[1], ei[2] * v[0] - ei[0] * v[2], ei[0] * v[1] - ei[1] * v[0]];
        const eiCrossU = [ei[1] * u[2] - ei[2] * u[1], ei[2] * u[0] - ei[0] * u[2], ei[0] * u[1] - ei[1] * u[0]];
        const xCrossEiCrossV = [
            x[1] * eiCrossV[2] - x[2] * eiCrossV[1],
            x[2] * eiCrossV[0] - x[0] * eiCrossV[2],
            x[0] * eiCrossV[1] - x[1] * eiCrossV[0],
        ];
        const dTheta = theta < 1e-12 ? 0 : x[i] / theta;
        const dsI = sp * dTheta;
        const dc2I = c2p * dTheta;
        for (let row = 0; row < 3; row++) {
            out[row * 3 + i] = dsI * u[row] + s * eiCrossV[row] + dc2I * w[row] + c2 * (eiCrossU[row] + xCrossEiCrossV[row]);
        }
    }
    return out;
}

function mat3Vec(m, v, out) {
    for (let row = 0; row < 3; row++) {
        out[row] = m[row * 3] * v[0] + m[row * 3 + 1] * v[1] + m[row * 3 + 2] * v[2];
    }
    return out;
}

function mat3Multiply(out, a, b) {
    for (let i = 0; i < 3; i++) {
        for (let j = 0; j < 3; j++) {
            out[i * 3 + j] =
                a[i * 3] * b[j] + a[i * 3 + 1] * b[3 + j] + a[i * 3 + 2] * b[6 + j];
        }
    }
    return out;
}

function mat3Transpose(out, a) {
    out[0] = a[0];
    out[1] = a[3];
    out[2] = a[6];
    out[3] = a[1];
    out[4] = a[4];
    out[5] = a[7];
    out[6] = a[2];
    out[7] = a[5];
    out[8] = a[8];
    return out;
}

// ---------------------------------------------------------------------------
// Shared surface skinning. Used both by the solver and GLTFSkin.sampleSurface.
// A vertex is skinned per stored influence (no weight renormalization) and only
// afterwards are the three vertices mixed with barycentric coordinates.
// ---------------------------------------------------------------------------

function readTriangle(geometry, triangle) {
    const indexAttr = geometry.attributes.index;
    const data = indexAttr.data;
    const first = triangle * 3;
    if (data instanceof Uint16Array || data instanceof Uint32Array) {
        return [data[first], data[first + 1], data[first + 2]];
    }
    fail('geometry index must be a Uint16Array or Uint32Array');
}

// Live surface sampling against current joint world matrices is provided by
// GLTFSkin.sampleSurface, which shares its palette assembly with
// updateUniforms. The solver below builds the same worldMatrix * bindInverse
// palette on shadow copies so no GPU state is ever touched.

// ---------------------------------------------------------------------------
// Validation and problem preparation
// ---------------------------------------------------------------------------

function isDescendantOf(node, ancestor) {
    let cur = node;
    while (cur) {
        if (cur === ancestor) return true;
        cur = cur.parent;
    }
    return false;
}

function validateGeometry(skin, skelJoints) {
    const geometry = skin.geometry;
    if (!geometry || !geometry.attributes) fail('skin must expose a geometry');
    const { position, skinIndex, skinWeight, index } = geometry.attributes;
    if (!position || !(position.data instanceof Float32Array) || position.size !== 3) fail('position must be a tight Float32 attribute of size 3');
    if (position.stride || position.offset) fail('position must be tightly packed');
    if (position.data.length !== position.count * 3) fail('position must be tightly packed');
    if (!skinIndex || !(skinIndex.data instanceof Float32Array) || skinIndex.size !== 4) fail('skinIndex must be a Float32 attribute of size 4');
    if (!skinWeight || !(skinWeight.data instanceof Float32Array) || skinWeight.size !== 4) fail('skinWeight must be a Float32 attribute of size 4');
    if (skinIndex.data.length !== skinIndex.count * 4 || skinWeight.data.length !== skinWeight.count * 4) fail('skin attributes must be tightly packed');
    if (!index || !(index.data instanceof Uint16Array || index.data instanceof Uint32Array)) fail('index must be Uint16Array or Uint32Array');
    if (position.count !== skinIndex.count || position.count !== skinWeight.count) fail('position/skin attributes must share vertex count');
    if (skelJoints.length > 64) fail('at most 64 skeleton joints are supported');
    const vertexCount = position.count;
    const posData = position.data;
    const idxData = skinIndex.data;
    const wData = skinWeight.data;
    for (let v = 0; v < vertexCount; v++) {
        for (let d = 0; d < 3; d++) {
            if (!isFiniteNumber(posData[v * 3 + d])) fail(`position vertex ${v} contains non-finite data`);
        }
        let sum = 0;
        for (let slot = 0; slot < 4; slot++) {
            const j = idxData[v * 4 + slot];
            const w = wData[v * 4 + slot];
            if (!Number.isInteger(j) || j < 0 || j >= skelJoints.length) fail(`vertex ${v} slot ${slot}: skinIndex ${j} out of range`);
            if (!isFiniteNumber(w) || w < 0) fail(`vertex ${v} slot ${slot}: skinWeight must be finite and non-negative`);
            sum += w;
        }
        if (Math.abs(sum - 1) > 2e-7) fail(`vertex ${v}: skinWeight sum ${sum} differs from 1 by more than 2e-7`);
    }
    const indexCount = index.data.length;
    if (!Number.isInteger(indexCount / 3)) fail('index count must be a multiple of 3');
}

function sampleChannel(channel, t, size, out) {
    const times = channel.times;
    const values = channel.values;
    if (t <= times[0]) {
        for (let i = 0; i < size; i++) out[i] = values[i];
        return out;
    }
    const last = times.length - 1;
    if (t >= times[last]) {
        for (let i = 0; i < size; i++) out[i] = values[last * size + i];
        return out;
    }
    let lo = 0;
    let hi = last;
    while (hi - lo > 1) {
        const mid = (lo + hi) >> 1;
        if (times[mid] <= t) lo = mid;
        else hi = mid;
    }
    const alpha = (t - times[lo]) / (times[hi] - times[lo]);
    if (size === 3) {
        for (let i = 0; i < 3; i++) out[i] = values[lo * 3 + i] * (1 - alpha) + values[hi * 3 + i] * alpha;
    } else {
        const a = [0, 0, 0, 0];
        const b = [0, 0, 0, 0];
        for (let i = 0; i < 4; i++) {
            a[i] = values[lo * 4 + i];
            b[i] = values[hi * 4 + i];
        }
        qSlerp(out, a, b, alpha);
    }
    return out;
}

function prepareProblem(animation, options) {
    if (!animation || !Array.isArray(animation.data)) fail('animation must be a GLTFAnimation instance');
    if (!options || typeof options !== 'object') fail('options object is required');
    const { skin, root, joints, times, probes, observations, lambda, smoothness, maxAngle } = options;
    const tolerance = options.tolerance === undefined ? 1e-6 : options.tolerance;
    const maxIterations = options.maxIterations === undefined ? 1000 : options.maxIterations;

    if (!skin || !skin.skeleton || Array.isArray(skin.skeleton.joints) === false) fail('skin must be a GLTFSkin with a skeleton');
    const skelJoints = skin.skeleton.joints;
    if (!skelJoints.length) fail('skeleton has no joints');
    if (!root || typeof root !== 'object' || root.parent === undefined) fail('root must be a Transform');
    if (!Array.isArray(joints) || joints.length < 1 || joints.length > 12) fail('joints must contain between 1 and 12 entries');
    if (new Set(joints).size !== joints.length) fail('joints must be distinct');
    const adjustableIndex = new Map();
    const skelIndex = new Map();
    skelJoints.forEach((j, i) => skelIndex.set(j, i));
    for (const joint of joints) {
        if (joint === root) fail('root must not be an adjustable joint');
        if (!skelIndex.has(joint)) fail('every adjustable joint must belong to skin.skeleton.joints');
        if (!isDescendantOf(joint, root)) fail('every skeleton joint must be a descendant of root');
        adjustableIndex.set(joint, adjustableIndex.size);
    }
    for (const joint of skelJoints) {
        if (!isDescendantOf(joint, root)) fail('every skeleton joint must be a descendant of root');
        if (!joint.bindInverse || joint.bindInverse.length !== 16) fail('every skeleton joint needs a 16-component bindInverse');
    }
    if (!Array.isArray(times) || times.length < 3 || times.length > 32) fail('times must contain between 3 and 32 samples');
    const N = times.length;
    const sampleTimes = new Float64Array(N);
    for (let i = 0; i < N; i++) {
        if (!isFiniteNumber(times[i])) fail('times must be finite numbers');
        sampleTimes[i] = times[i];
        if (i > 0 && sampleTimes[i] <= sampleTimes[i - 1]) fail('times must be strictly increasing');
    }
    if (sampleTimes[0] !== animation.startTime || sampleTimes[N - 1] !== animation.endTime) {
        fail('times endpoints must equal animation.startTime / animation.endTime');
    }
    if (!Array.isArray(probes) || !probes.length) fail('probes must be a non-empty array');
    if (!Array.isArray(observations) || !observations.length) fail('observations must be a non-empty array');
    if (!isPositiveFinite(lambda)) fail('lambda must be a positive finite number');
    if (!isPositiveFinite(smoothness)) fail('smoothness must be a positive finite number');
    if (!isFiniteNumber(maxAngle) || maxAngle <= 0 || maxAngle >= Math.PI / 2) fail('maxAngle must lie strictly inside (0, pi/2)');
    if (!isPositiveFinite(tolerance)) fail('tolerance must be a positive finite number');
    if (!Number.isInteger(maxIterations) || maxIterations <= 0) fail('maxIterations must be a positive integer');

    validateGeometry(skin, skelJoints);

    // Channels: only LINEAR position / quaternion, no duplicates, all nodes in
    // the root subtree.
    const channelMap = new Map();
    for (const channel of animation.data) {
        if (!channel || channel.interpolation !== 'LINEAR') fail(`only LINEAR channels are supported (got ${channel && channel.interpolation})`);
        if (channel.transform !== 'position' && channel.transform !== 'quaternion') fail(`only position/quaternion channels are supported (got ${channel.transform})`);
        const node = channel.node;
        if (!node || !isDescendantOf(node, root)) fail('all animated nodes must live inside the root subtree');
        const set = channelMap.get(node) || new Set();
        if (set.has(channel.transform)) fail(`duplicate ${channel.transform} channel on the same node`);
        set.add(channel.transform);
        if (!channelMap.has(node)) channelMap.set(node, set);
        const size = channel.transform === 'position' ? 3 : 4;
        if (!channel.times || !channel.values || channel.times.length < 2) fail('channels need at least two keys');
        if (channel.values.length !== channel.times.length * size) fail('channel values length does not match times');
        for (let i = 0; i < channel.times.length; i++) {
            if (!isFiniteNumber(channel.times[i]) || (i > 0 && channel.times[i] <= channel.times[i - 1])) fail('channel times must be strictly increasing finite numbers');
        }
        for (let i = 0; i < channel.values.length; i++) {
            if (!isFiniteNumber(channel.values[i])) fail('channel values must be finite');
        }
        if (size === 4) {
            for (let k = 0; k < channel.times.length; k++) {
                let norm = 0;
                for (let d = 0; d < 4; d++) norm += channel.values[k * 4 + d] ** 2;
                if (Math.abs(norm - 1) > 1e-5) fail('quaternion keys must be unit quaternions');
            }
        }
    }

    // Subtree node snapshot. Local scale must be 1; local TRS is snapshotted so
    // missing channels fall back to call-start local values. Note Transform
    // traversal stops when the callback returns truthy, so push explicitly.
    const nodes = [];
    root.traverse((node) => {
        nodes.push(node);
    });
    const nodeIndex = new Map(nodes.map((node, i) => [node, i]));
    for (const node of nodes) {
        const s = node.scale;
        if (!s || s[0] !== 1 || s[1] !== 1 || s[2] !== 1) fail('all local scales inside the root subtree must be 1');
    }
    const U = nodes.length;
    const K = joints.length;

    // Fixed rigid transform of everything above root, captured at call start.
    const ancestorWorld = new Float64Array(16);
    if (root.parent) {
        for (let i = 0; i < 16; i++) ancestorWorld[i] = root.parent.worldMatrix[i];
    } else {
        ancestorWorld[0] = ancestorWorld[5] = ancestorWorld[10] = ancestorWorld[15] = 1;
    }

    // Base local poses per sample (original animation, no corrections yet).
    const basePos = new Float64Array(N * U * 3);
    const baseQuat = new Float64Array(N * U * 4);
    const tmp3 = [0, 0, 0];
    const tmp4 = [0, 0, 0, 0];
    for (let u = 0; u < U; u++) {
        const node = nodes[u];
        const posChannel = animation.data.find((c) => c.node === node && c.transform === 'position');
        const quatChannel = animation.data.find((c) => c.node === node && c.transform === 'quaternion');
        for (let s = 0; s < N; s++) {
            const t = sampleTimes[s];
            const pOut = basePos.subarray((s * U + u) * 3, (s * U + u) * 3 + 3);
            const qOut = baseQuat.subarray((s * U + u) * 4, (s * U + u) * 4 + 4);
            if (posChannel) sampleChannel(posChannel, t, 3, tmp3);
            else tmp3[0] = node.position[0], tmp3[1] = node.position[1], tmp3[2] = node.position[2];
            pOut[0] = tmp3[0];
            pOut[1] = tmp3[1];
            pOut[2] = tmp3[2];
            if (quatChannel) sampleChannel(quatChannel, t, 4, tmp4);
            else {
                tmp4[0] = node.quaternion[0];
                tmp4[1] = node.quaternion[1];
                tmp4[2] = node.quaternion[2];
                tmp4[3] = node.quaternion[3];
            }
            // Keep the raw sampled values so the output's end keys reproduce
            // the original channel bits exactly; normalization for the shadow
            // kinematics happens inside composeWorlds.
            qOut[0] = tmp4[0];
            qOut[1] = tmp4[1];
            qOut[2] = tmp4[2];
            qOut[3] = tmp4[3];
        }
    }

    // Probes -> concrete vertices and barycentric weights.
    const geometry = skin.geometry;
    const indexCount = geometry.attributes.index.data.length;
    const probeVerts = [];
    const usedVertices = new Set();
    probes.forEach((probe, pi) => {
        if (!probe || !isFiniteNumber(probe.triangle)) fail(`probe ${pi}: triangle must be a finite number`);
        const tri = probe.triangle;
        if (!Number.isInteger(tri) || tri < 0 || tri * 3 + 2 >= indexCount) fail(`probe ${pi}: triangle ${tri} out of range`);
        const bary = probe.barycentric;
        if (!bary || bary.length !== 3) fail(`probe ${pi}: barycentric must have three components`);
        let bSum = 0;
        for (let i = 0; i < 3; i++) {
            if (!isFiniteNumber(bary[i]) || bary[i] < 0) fail(`probe ${pi}: barycentric weights must be finite and non-negative`);
            bSum += bary[i];
        }
        if (Math.abs(bSum - 1) > 1e-9) fail(`probe ${pi}: barycentric weights must sum to 1`);
        const verts = readTriangle(geometry, tri);
        probeVerts.push({ verts, bary: [bary[0], bary[1], bary[2]] });
        verts.forEach((v) => usedVertices.add(v));
    });
    const vertexList = Array.from(usedVertices).sort((a, b) => a - b);
    const vertexSlot = new Map(vertexList.map((v, i) => [v, i]));
    const positionData = geometry.attributes.position.data;
    const skinIndexData = geometry.attributes.skinIndex.data;
    const skinWeightData = geometry.attributes.skinWeight.data;
    const influences = vertexList.map((v) => {
        const list = [];
        for (let slot = 0; slot < 4; slot++) {
            const w = skinWeightData[v * 4 + slot];
            if (w) list.push({ j: skinIndexData[v * 4 + slot], w });
        }
        return list;
    });
    const bindPoints = vertexList.map((v) => [positionData[v * 3], positionData[v * 3 + 1], positionData[v * 3 + 2]]);

    const obsList = [];
    observations.forEach((obs, oi) => {
        if (!obs || typeof obs !== 'object') fail(`observation ${oi} is invalid`);
        if (!Number.isInteger(obs.sample) || obs.sample < 0 || obs.sample >= N) fail(`observation ${oi}: sample out of range`);
        if (!Number.isInteger(obs.probe) || obs.probe < 0 || obs.probe >= probes.length) fail(`observation ${oi}: probe out of range`);
        if (!obs.target || obs.target.length < 3 || ![0, 1, 2].every((d) => isFiniteNumber(obs.target[d]))) fail(`observation ${oi}: target must be a finite 3D point`);
        if (!isPositiveFinite(obs.weight)) fail(`observation ${oi}: weight must be positive finite`);
        obsList.push({ s: obs.sample, p: obs.probe, target: [obs.target[0], obs.target[1], obs.target[2]], w: obs.weight });
    });

    // Adjustable joint metadata.
    const J = skelJoints.length;
    const adjustableSkel = joints.map((joint) => skelIndex.get(joint));
    const ancestorFlag = Array.from({ length: K }, () => new Float64Array(J));
    for (let k = 0; k < K; k++) {
        for (let j = 0; j < J; j++) {
            ancestorFlag[k][j] = isDescendantOf(skelJoints[j], joints[k]) ? 1 : 0;
        }
    }
    const bindInverse = skelJoints.map((joint) => Float64Array.from(joint.bindInverse));

    // Non-uniform time discretization.
    const d = new Float64Array(N - 1);
    for (let s = 0; s < N - 1; s++) d[s] = sampleTimes[s + 1] - sampleTimes[s];
    const h = new Float64Array(N - 2);
    for (let s = 1; s < N - 1; s++) h[s - 1] = (d[s - 1] + d[s]) * 0.5;

    return {
        animation,
        skin,
        root,
        joints,
        nodes,
        nodeIndex,
        U,
        K,
        J,
        N,
        sampleTimes,
        d,
        h,
        lambda,
        smoothness,
        maxAngle,
        tolerance,
        maxIterations,
        ancestorWorld,
        basePos,
        baseQuat,
        skelJoints,
        adjustableSkel,
        ancestorFlag,
        bindInverse,
        probeVerts,
        vertexList,
        vertexSlot,
        influences,
        bindPoints,
        obsList,
    };
}

// ---------------------------------------------------------------------------
// Shadow hierarchy + objective / analytic gradient evaluation
// ---------------------------------------------------------------------------

function composeWorlds(ctx, x) {
    const { N, U, J, nodes, nodeIndex, ancestorWorld, basePos, baseQuat, skelJoints, bindInverse } = ctx;
    const worldPool = new Float64Array(N * U * 16);
    const palettePool = new Float64Array(N * J * 16);
    const local = new Float64Array(16);
    const expQ = [0, 0, 0, 0];
    const corrQ = [0, 0, 0, 0];
    const unitQ = [0, 0, 0, 0];
    for (let s = 0; s < N; s++) {
        for (let u = 0; u < U; u++) {
            const node = nodes[u];
            const p = basePos.subarray((s * U + u) * 3, (s * U + u) * 3 + 3);
            const baseQ = baseQuat.subarray((s * U + u) * 4, (s * U + u) * 4 + 4);
            let q = qNormalize(unitQ, baseQ);
            const adjK = ctx.jointToAdjustable.get(node);
            if (adjK !== undefined && s > 0 && s < N - 1) {
                const xv = x.subarray(((s - 1) * ctx.K + adjK) * 3, ((s - 1) * ctx.K + adjK + 1) * 3);
                qExp(expQ, xv);
                qMultiply(corrQ, q, expQ);
                q = corrQ;
            }
            m4ComposeTR(local, p, q);
            const world = worldPool.subarray((s * U + u) * 16, (s * U + u) * 16 + 16);
            if (node === ctx.root) {
                m4Multiply(world, ancestorWorld, local);
            } else {
                const pu = nodeIndex.get(node.parent);
                const parentWorld = worldPool.subarray((s * U + pu) * 16, (s * U + pu) * 16 + 16);
                m4Multiply(world, parentWorld, local);
            }
        }
        for (let j = 0; j < J; j++) {
            const u = nodeIndex.get(skelJoints[j]);
            const world = worldPool.subarray((s * U + u) * 16, (s * U + u) * 16 + 16);
            const palette = palettePool.subarray((s * J + j) * 16, (s * J + j + 1) * 16);
            m4Multiply(palette, world, bindInverse[j]);
        }
    }
    return { worldPool, palettePool };
}

// Surface Jacobian of one probe w.r.t. every adjustable joint twist at one
// sample. For joint k, the influenced point sits at a constant (for this
// partial derivative) base-local arm a relative to k, and its corrected world
// position is t_k + R_k0 R(x_k) a, so d c / d x_k = R_k0 dExp(x_k)[a], where
// R_k0 = R_k_corrected R(x_k)^T is the fixed base world rotation and
// a = R_k_corrected^T (c - t_k). Barycentric mixing and the stored skin
// weights are summed per influence, so shared ancestors and multi-bone points
// are handled exactly.
function observationJacobians(ctx, x, s, probeIndex, worldPool, palettePool) {
    const { K, J, U, nodeIndex, skelJoints, adjustableSkel, ancestorFlag, probeVerts, vertexSlot, influences, bindPoints } = ctx;
    const probe = probeVerts[probeIndex];
    const Js = Array.from({ length: K }, () => new Float64Array(9));
    const rkT = new Float64Array(9);
    const arm = [0, 0, 0];
    const dvec = [0, 0, 0];
    const worldJac = new Float64Array(9);
    const tmp = [0, 0, 0];
    const expQ = [0, 0, 0, 0];
    const rNeg4 = new Float64Array(16);
    const rNeg = new Float64Array(9);
    const rk0 = new Float64Array(9);
    for (let k = 0; k < K; k++) {
        const skelK = adjustableSkel[k];
        const ku = nodeIndex.get(skelJoints[skelK]);
        const worldK = worldPool.subarray((s * U + ku) * 16, (s * U + ku) * 16 + 16);
        // Row-major 3x3 extracted from the column-major mat4 rotation block.
        const rk = [worldK[0], worldK[4], worldK[8], worldK[1], worldK[5], worldK[9], worldK[2], worldK[6], worldK[10]];
        mat3Transpose(rkT, rk);
        const xk = x.subarray(((s - 1) * K + k) * 3, ((s - 1) * K + k) * 3 + 3);
        // Base rotation R_k0 = R_k_corrected * R(x_k)^T.
        qExp(expQ, [-xk[0], -xk[1], -xk[2]]);
        m4FromQuat(rNeg4, expQ);
        rNeg[0] = rNeg4[0];
        rNeg[1] = rNeg4[4];
        rNeg[2] = rNeg4[8];
        rNeg[3] = rNeg4[1];
        rNeg[4] = rNeg4[5];
        rNeg[5] = rNeg4[9];
        rNeg[6] = rNeg4[2];
        rNeg[7] = rNeg4[6];
        rNeg[8] = rNeg4[10];
        mat3Multiply(rk0, rk, rNeg);
        for (let vi2 = 0; vi2 < 3; vi2++) {
            const slot = vertexSlot.get(probe.verts[vi2]);
            const b = probe.bary[vi2];
            for (const inf of influences[slot]) {
                if (!ancestorFlag[k][inf.j]) continue;
                const palette = palettePool.subarray((s * J + inf.j) * 16, (s * J + inf.j + 1) * 16);
                const cSingle = m4TransformPoint(palette, bindPoints[slot], tmp);
                dvec[0] = cSingle[0] - worldK[12];
                dvec[1] = cSingle[1] - worldK[13];
                dvec[2] = cSingle[2] - worldK[14];
                mat3Vec(rkT, dvec, arm);
                const localJac = expPointJacobian(xk, arm);
                mat3Multiply(worldJac, rk0, localJac);
                const scale = b * inf.w;
                for (let i = 0; i < 9; i++) Js[k][i] += scale * worldJac[i];
            }
        }
    }
    return Js;
}

function evaluate(ctx, x) {
    const { N, U, J, K, nodeIndex, skelJoints, probeVerts, vertexList, vertexSlot, influences, bindPoints, obsList, d, h, lambda, smoothness } = ctx;
    const { worldPool, palettePool } = composeWorlds(ctx, x);

    const surfaces = new Float64Array(N * probeVerts.length * 3);
    const errors = new Float64Array(obsList.length);
    let dataE = 0;

    // Per (sample, vertex) skinned world points.
    const skinned = new Float64Array(N * vertexList.length * 3);
    const tmp = [0, 0, 0];
    for (let s = 0; s < N; s++) {
        for (let vi = 0; vi < vertexList.length; vi++) {
            const out = skinned.subarray((s * vertexList.length + vi) * 3, (s * vertexList.length + vi + 1) * 3);
            const p = bindPoints[vi];
            for (const inf of influences[vi]) {
                const palette = palettePool.subarray((s * J + inf.j) * 16, (s * J + inf.j + 1) * 16);
                m4TransformPoint(palette, p, tmp);
                out[0] += inf.w * tmp[0];
                out[1] += inf.w * tmp[1];
                out[2] += inf.w * tmp[2];
            }
        }
        probeVerts.forEach((probe, pi) => {
            const out = surfaces.subarray((s * probeVerts.length + pi) * 3, (s * probeVerts.length + pi + 1) * 3);
            for (let i = 0; i < 3; i++) {
                const vi = vertexSlot.get(probe.verts[i]);
                const sp = skinned.subarray((s * vertexList.length + vi) * 3, (s * vertexList.length + vi + 1) * 3);
                const b = probe.bary[i];
                out[0] += b * sp[0];
                out[1] += b * sp[1];
                out[2] += b * sp[2];
            }
        });
    }

    // Residuals and data objective.
    const residualVec = obsList.map((obs) => {
        const o = (obs.s * probeVerts.length + obs.p) * 3;
        const e = [surfaces[o] - obs.target[0], surfaces[o + 1] - obs.target[1], surfaces[o + 2] - obs.target[2]];
        return e;
    });
    obsList.forEach((obs, oi) => {
        const e = residualVec[oi];
        const norm2 = e[0] * e[0] + e[1] * e[1] + e[2] * e[2];
        errors[oi] = Math.sqrt(norm2);
        dataE += 0.5 * obs.w * norm2;
    });

    const nBlocks = (N - 2) * K;
    const g = new Float64Array(nBlocks * 3);

    // Data-term analytic gradient via the full Exp differential.
    for (let oi = 0; oi < obsList.length; oi++) {
        const obs = obsList[oi];
        const s = obs.s;
        if (s === 0 || s === N - 1) continue; // end frames are fixed
        const e = residualVec[oi];
        const Js = observationJacobians(ctx, x, s, obs.p, worldPool, palettePool);
        const frameBase = (s - 1) * K;
        for (let k = 0; k < K; k++) {
            const go = (frameBase + k) * 3;
            // J[row*3+col] = d(surface_row)/d(x_col); the gradient is J^T e,
            // so variable component col sums over the three output rows.
            for (let col = 0; col < 3; col++) {
                let dot = 0;
                for (let row = 0; row < 3; row++) dot += Js[k][row * 3 + col] * e[row];
                g[go + col] += obs.w * dot;
            }
        }
    }

    // Tikhonov term on the correction twists (internal frames only).
    let regE = 0;
    for (let i = 0; i < x.length; i++) {
        regE += 0.5 * lambda * x[i] * x[i];
        g[i] += lambda * x[i];
    }

    // Discrete-time acceleration smoothness on the corrections.
    let smoothE = 0;
    for (let k = 0; k < K; k++) {
        for (let s = 1; s < N - 1; s++) {
            const hs = h[s - 1];
            const dm = d[s - 1];
            const dp = d[s];
            const xm = s === 1 ? ZERO3 : x.subarray(((s - 2) * K + k) * 3, ((s - 2) * K + k + 1) * 3);
            const xc = x.subarray(((s - 1) * K + k) * 3, ((s - 1) * K + k + 1) * 3);
            const xp = s === N - 2 ? ZERO3 : x.subarray((s * K + k) * 3, (s * K + k + 1) * 3);
            const a = [0, 0, 0];
            for (let dim = 0; dim < 3; dim++) {
                a[dim] = ((xp[dim] - xc[dim]) / dp - (xc[dim] - xm[dim]) / dm) / hs;
            }
            const norm2 = a[0] * a[0] + a[1] * a[1] + a[2] * a[2];
            smoothE += 0.5 * smoothness * hs * norm2;
            const gm = smoothness * hs;
            const cp = 1 / (hs * dp);
            const cm = 1 / (hs * dm);
            const c0 = -(cp + cm);
            const addTo = (block, coef, vec) => {
                // End frames are fixed: neither negative nor out-of-range frame
                // numbers belong to the internal-variable gradient.
                if (block < 0 || block > N - 3) return;
                const o = (block * K + k) * 3;
                g[o] += gm * coef * vec[0];
                g[o + 1] += gm * coef * vec[1];
                g[o + 2] += gm * coef * vec[2];
            };
            addTo(s - 2, cm, a);
            addTo(s - 1, c0, a);
            addTo(s, cp, a);
        }
    }

    return { E: dataE + regE + smoothE, dataE, regE, smoothE, g, surfaces, errors, worldPool, palettePool };
}

const ZERO3 = [0, 0, 0];

// Ball projection for one 3D twist block.
function projectBlock(x, offset, radius) {
    const a = x[offset];
    const b = x[offset + 1];
    const c = x[offset + 2];
    const len = Math.hypot(a, b, c);
    if (len > radius) {
        const s = radius / len;
        x[offset] = a * s;
        x[offset + 1] = b * s;
        x[offset + 2] = c * s;
    }
}

function projectAll(x, radius) {
    for (let i = 0; i < x.length; i += 3) projectBlock(x, i, radius);
}

// Stationarity residual against the projected ball: max |x - P(x - g)|.
function stationarityResidual(x, g, radius) {
    let residual = 0;
    for (let i = 0; i < x.length; i += 3) {
        const y0 = x[i] - g[i];
        const y1 = x[i + 1] - g[i + 1];
        const y2 = x[i + 2] - g[i + 2];
        const len = Math.hypot(y0, y1, y2);
        const p0 = len > radius ? (radius / len) * y0 : y0;
        const p1 = len > radius ? (radius / len) * y1 : y1;
        const p2 = len > radius ? (radius / len) * y2 : y2;
        residual = Math.max(residual, Math.abs(x[i] - p0), Math.abs(x[i + 1] - p1), Math.abs(x[i + 2] - p2));
    }
    return residual;
}

// Gauss-Newton Hessian approximation (data term) plus the exact Hessians of
// the quadratic regularization and smoothness terms. Stored as 3x3 blocks.
function buildHessianBlocks(ctx, x, evalResult) {
    const { N, K, J, U, nodeIndex, skelJoints, probeVerts, vertexSlot, influences, bindPoints, obsList, d, h, lambda, smoothness, ancestorFlag, adjustableSkel } = ctx;
    const { worldPool, palettePool } = evalResult;
    const nBlocks = (N - 2) * K;
    const blocks = new Map();
    // Canonical upper-triangle keys: the CG matrix-vector product mirrors the
    // off-diagonal block, so lower entries must not be stored separately.
    const blockKey = (r, c) => (r <= c ? `${r}:${c}` : `${c}:${r}`);
    const addBlock = (r, c, m) => {
        if (r < 0 || c < 0 || r >= nBlocks || c >= nBlocks) return;
        const key = blockKey(r, c);
        let entry = blocks.get(key);
        if (!entry) {
            entry = new Float64Array(9);
            blocks.set(key, entry);
        }
        for (let i = 0; i < 9; i++) entry[i] += m[i];
    };

    // Data GN blocks built from the same full Exp-differential Jacobians that
    // feed the analytic gradient: H += w J^T J.
    for (const obs of obsList) {
        const s = obs.s;
        if (s === 0 || s === N - 1) continue;
        const frameBase = s - 1;
        const Js = observationJacobians(ctx, x, s, obs.p, worldPool, palettePool);
        for (let k1 = 0; k1 < K; k1++) {
            for (let k2 = 0; k2 < K; k2++) {
                const m = new Float64Array(9);
                const a = Js[k1];
                const b = Js[k2];
                for (let i = 0; i < 3; i++) {
                    for (let j = 0; j < 3; j++) {
                        let sum = 0;
                        for (let z = 0; z < 3; z++) sum += a[z * 3 + i] * b[z * 3 + j];
                        m[i * 3 + j] = obs.w * sum;
                    }
                }
                addBlock(frameBase * K + k1, frameBase * K + k2, m);
            }
        }
    }

    // lambda diagonal blocks.
    for (let b2 = 0; b2 < nBlocks; b2++) {
        const m = new Float64Array(9);
        m[0] = m[4] = m[8] = lambda;
        addBlock(b2, b2, m);
    }

    // Smoothness Hessian: scalar tridiagonal coefficients repeated on I3.
    for (let k = 0; k < K; k++) {
        for (let s = 1; s < N - 1; s++) {
            const hs = h[s - 1];
            const cp = 1 / (hs * d[s]);
            const cm = 1 / (hs * d[s - 1]);
            const c0 = -(cp + cm);
            const coefs = [
                { frame: s - 2, c: cm },
                { frame: s - 1, c: c0 },
                { frame: s, c: cp },
            ];
            for (const u of coefs) {
                for (const v of coefs) {
                    if (u.frame < 0 || v.frame < 0) continue;
                    const m = new Float64Array(9);
                    const val = smoothness * hs * u.c * v.c;
                    m[0] = m[4] = m[8] = val;
                    addBlock(u.frame * K + k, v.frame * K + k, m);
                }
            }
        }
    }
    return blocks;
}

// Conjugate gradient solve on block-sparse symmetric (upper-stored) Hessian.
function cgSolve(blocks, nBlocks, rhs, cap) {
    const n = nBlocks * 3;
    const x = new Float64Array(n);
    const r = Float64Array.from(rhs);
    const p = Float64Array.from(rhs);
    const rsOld0 = r.reduce((a, v) => a + v * v, 0);
    let rsOld = rsOld0;
    if (rsOld === 0) return x;
    const matVec = (vec, out) => {
        out.fill(0);
        for (const [key, m] of blocks) {
            const [br, bc] = key.split(':').map(Number);
            const ro = br * 3;
            const co = bc * 3;
            for (let i = 0; i < 3; i++) {
                let sum = 0;
                for (let j = 0; j < 3; j++) sum += m[i * 3 + j] * vec[co + j];
                out[ro + i] += sum;
                if (br !== bc) {
                    let sum2 = 0;
                    for (let j = 0; j < 3; j++) sum2 += m[j * 3 + i] * vec[ro + j];
                    out[co + i] += sum2;
                }
            }
        }
    };
    const Ap = new Float64Array(n);
    const iterations = Math.min(cap, n * 2);
    for (let it = 0; it < iterations; it++) {
        matVec(p, Ap);
        let pAp = 0;
        for (let i = 0; i < n; i++) pAp += p[i] * Ap[i];
        if (!(pAp > 0)) break;
        const alpha = rsOld / pAp;
        for (let i = 0; i < n; i++) {
            x[i] += alpha * p[i];
            r[i] -= alpha * Ap[i];
        }
        const rsNew = r.reduce((a, v) => a + v * v, 0);
        if (Math.sqrt(rsNew) < 1e-12 * (1 + Math.sqrt(rsOld))) break;
        const beta = rsNew / rsOld;
        for (let i = 0; i < n; i++) p[i] = r[i] + beta * p[i];
        rsOld = rsNew;
    }
    return x;
}

// ---------------------------------------------------------------------------
// Output animation
// ---------------------------------------------------------------------------

function buildResultAnimation(ctx, x) {
    const { N, U, K, nodes, nodeIndex, sampleTimes, basePos, baseQuat } = ctx;
    const data = [];
    const expQ = [0, 0, 0, 0];
    const finalQ = [0, 0, 0, 0];
    for (let u = 0; u < U; u++) {
        const node = nodes[u];
        const adjK = ctx.jointToAdjustable.get(node);
        const posValues = new Float32Array(N * 3);
        const quatValues = new Float32Array(N * 4);
        for (let s = 0; s < N; s++) {
            const p = basePos.subarray((s * U + u) * 3, (s * U + u) * 3 + 3);
            const q = baseQuat.subarray((s * U + u) * 4, (s * U + u) * 4 + 4);
            posValues.set(p, s * 3);
            if (adjK !== undefined) {
                if (s === 0 || s === N - 1) {
                    quatValues.set(q, s * 4); // exact end-pose preservation
                } else {
                    const xv = x.subarray(((s - 1) * K + adjK) * 3, ((s - 1) * K + adjK + 1) * 3);
                    qExp(expQ, xv);
                    qMultiply(finalQ, q, expQ);
                    quatValues.set(finalQ, s * 4);
                }
            } else {
                quatValues.set(q, s * 4);
            }
        }
        const timesCopy = new Float32Array(sampleTimes);
        data.push({ node, transform: 'position', interpolation: 'LINEAR', times: timesCopy, values: posValues });
        data.push({ node, transform: 'quaternion', interpolation: 'LINEAR', times: new Float32Array(sampleTimes), values: quatValues });
    }
    const animation = new GLTFAnimation(data, 1);
    animation.elapsed = 0;
    animation.loop = false;
    return animation;
}

// ---------------------------------------------------------------------------
// Main entry
// ---------------------------------------------------------------------------

export function editSkinMotion(animation, options) {
    return editSkinMotionImpl(animation, options);
}

function editSkinMotionImpl(animation, options) {
    const ctx = prepareProblem(animation, options);
    // Map node -> adjustable slot, used by pose construction.
    ctx.jointToAdjustable = new Map();
    ctx.adjustableIndex = new Map();
    ctx.joints.forEach((joint, k) => {
        ctx.jointToAdjustable.set(joint, k);
        ctx.adjustableIndex.set(joint, k);
    });

    const { N, K, maxAngle, tolerance, maxIterations } = ctx;
    const nBlocks = (N - 2) * K;
    const n = nBlocks * 3;
    let x = new Float64Array(n);

    let initial = evaluate(ctx, x);
    const history = [initial.E];
    let bestEval = initial;
    let prevX = null;
    let prevG = null;
    let bbAlpha = 1;

    const candidate = new Float64Array(n);
    let iterations = 0;
    let converged = stationarityResidual(x, initial.g, maxAngle) <= tolerance;

    // Projected backtracking line search. Every accepted point must not
    // increase the full objective; Armijo gives the quadratic model a fair
    // chance before shrinking the step. When Armijo cannot be satisfied (for
    // example right at the optimum where model progress is negligible), the
    // contract relaxes to plain non-increase, which is all the solver requires.
    const lineSearch = (dir, startAlpha) => {
        const dirG = bestEval.g.reduce((acc, gi, i) => acc + gi * dir[i], 0);
        if (!(dirG < 0)) return null;
        const armijo = 1e-4;
        let alpha = startAlpha;
        let plainCandidate = null;
        let plainEval = null;
        for (let attempt = 0; attempt < 80; attempt++) {
            candidate.set(x);
            for (let i = 0; i < n; i++) candidate[i] += alpha * dir[i];
            projectAll(candidate, maxAngle);
            if (!candidate.every(Number.isFinite)) {
                alpha *= 0.5;
                continue;
            }
            if (candidate.every((v, i) => v === x[i])) return plainCandidate ? { ev: plainEval, usePlain: true } : null;
            const ev = evaluate(ctx, candidate);
            if (!Number.isFinite(ev.E)) {
                alpha *= 0.5;
                continue;
            }
            if (ev.E <= bestEval.E) {
                if (ev.E <= bestEval.E + armijo * alpha * dirG) return { ev };
                if (!plainCandidate || ev.E < plainEval.E) {
                    plainCandidate = Float64Array.from(candidate);
                    plainEval = ev;
                }
            }
            // Quadratic interpolation of the first failure, plain halving after.
            if (attempt === 0) {
                const denom = 2 * (bestEval.E + alpha * dirG - ev.E);
                if (denom > 0) {
                    const interp = (-alpha * alpha * dirG) / denom;
                    if (interp > 0 && interp < alpha) alpha = Math.max(interp, alpha * 0.1);
                    else alpha *= 0.5;
                } else alpha *= 0.5;
            } else {
                alpha *= 0.5;
            }
        }
        if (plainCandidate) {
            candidate.set(plainCandidate);
            return { ev: plainEval };
        }
        return null;
    };

    while (!converged && iterations < maxIterations) {
        iterations++;
        let step = null;

        // Primary direction: Gauss-Newton (data) plus exact quadratic
        // regularization/smoothness Hessian, solved with conjugate gradient.
        const blocks = buildHessianBlocks(ctx, x, bestEval);
        const negG = bestEval.g.map((v) => -v);
        const gnDir = cgSolve(blocks, nBlocks, negG, Math.min(120, n * 2));
        if (gnDir.some(Number.isFinite)) {
            step = lineSearch(gnDir, 1);
        }

        // Fallback: Barzilai-Borwein scaled projected gradient with the same
        // monotone Armijo contract.
        if (!step) {
            if (prevX && prevG) {
                let dxdx = 0;
                let dxdg = 0;
                for (let i = 0; i < n; i++) {
                    const dx = x[i] - prevX[i];
                    const dg = bestEval.g[i] - prevG[i];
                    dxdx += dx * dx;
                    dxdg += dx * dg;
                }
                if (dxdg > 0) bbAlpha = Math.min(1e3, Math.max(1e-6, dxdx / dxdg));
            }
            const gradDir = bestEval.g.map((v) => -bbAlpha * v);
            step = lineSearch(gradDir, 1);
            if (!step) {
                const plain = bestEval.g.map((v) => -v);
                step = lineSearch(plain, bbAlpha);
            }
        }

        if (!step) break;
        prevX = x;
        prevG = bestEval.g;
        x = Float64Array.from(candidate);
        bestEval = step.ev;
        history.push(bestEval.E);
        const residual = stationarityResidual(x, bestEval.g, maxAngle);
        if (residual <= tolerance) converged = true;
    }

    // Independent final recomputation: fresh evaluation and residual, never
    // reusing solver-side arrays.
    const finalX = Float64Array.from(x);
    const finalEval = evaluate(ctx, finalX);
    const finalResidual = stationarityResidual(finalX, finalEval.g, maxAngle);
    converged = finalResidual <= tolerance;

    const rotations = new Float64Array(N * K * 3);
    const gradient = new Float64Array(N * K * 3);
    for (let s = 1; s < N - 1; s++) {
        for (let k = 0; k < K; k++) {
            for (let dim = 0; dim < 3; dim++) {
                rotations[(s * K + k) * 3 + dim] = finalX[((s - 1) * K + k) * 3 + dim];
                gradient[(s * K + k) * 3 + dim] = finalEval.g[((s - 1) * K + k) * 3 + dim];
            }
        }
    }

    const resultAnimation = buildResultAnimation(ctx, finalX);

    return {
        animation: resultAnimation,
        rotations,
        gradient,
        objective: finalEval.E,
        residual: finalResidual,
        iterations,
        converged,
        history: history.slice(),
        errors: Float64Array.from(finalEval.errors),
    };
}

// Exposed for tests that want to validate the analytic gradient against finite
// differences or inspect the prepared shadow problem.
export const _skinMotionInternals = {
    prepareProblem,
    evaluate(ctx, x) {
        if (!ctx.jointToAdjustable) {
            ctx.jointToAdjustable = new Map();
            ctx.adjustableIndex = new Map();
            ctx.joints.forEach((joint, k) => {
                ctx.jointToAdjustable.set(joint, k);
                ctx.adjustableIndex.set(joint, k);
            });
        }
        return evaluate(ctx, Float64Array.from(x));
    },
    stationarityResidual,
    projectAll,
    qExp,
    observationJacobians,
    expPointJacobian,
};
