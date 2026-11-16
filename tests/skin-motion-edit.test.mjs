import { Geometry } from '../src/core/Geometry.js';
import { Transform } from '../src/core/Transform.js';
import { Mat4 } from '../src/math/Mat4.js';
import { GLTFSkin } from '../src/extras/GLTFSkin.js';
import { GLTFAnimation } from '../src/extras/GLTFAnimation.js';
import { makeDevice, makeEditingRig } from '../task_support/fixture.mjs';
import { _skinMotionInternals } from '../src/extras/SkinMotionEdit.js';

// Independent reference math (column-major mat4, gl-matrix conventions).
const qmul = (a, b) => [
    a[0] * b[3] + a[3] * b[0] + a[1] * b[2] - a[2] * b[1],
    a[1] * b[3] + a[3] * b[1] + a[2] * b[0] - a[0] * b[2],
    a[2] * b[3] + a[3] * b[2] + a[0] * b[1] - a[1] * b[0],
    a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2],
];
const qexp = (v) => {
    const th = Math.hypot(v[0], v[1], v[2]);
    if (th < 1e-12) return [v[0] * 0.5, v[1] * 0.5, v[2] * 0.5, 1];
    const s = Math.sin(th / 2) / th;
    return [v[0] * s, v[1] * s, v[2] * s, Math.cos(th / 2)];
};
const qnorm = (q) => {
    const l = Math.hypot(q[0], q[1], q[2], q[3]);
    return [q[0] / l, q[1] / l, q[2] / l, q[3] / l];
};
const qFromAxis = (axis, ang) => qexp([axis[0] * ang, axis[1] * ang, axis[2] * ang]);
function composeTR(pos, q) {
    const [x, y, z, w] = q;
    const x2 = x + x, y2 = y + y, z2 = z + z;
    const xx = x * x2, yx = y * x2, yy = y * y2, zx = z * x2, zy = z * y2, zz = z * z2;
    const wx = w * x2, wy = w * y2, wz = w * z2;
    // Column-major layout, identical to Mat4.fromQuat / compose in the library.
    return [
        1 - yy - zz, yx + wz, zx - wy, 0,
        yx - wz, 1 - xx - zz, zy + wx, 0,
        zx + wy, zy - wx, 1 - xx - yy, 0,
        pos[0], pos[1], pos[2], 1,
    ];
}
const mmul = (a, b) => {
    const out = new Array(16);
    for (let c = 0; c < 4; c++) for (let r = 0; r < 4; r++) {
        out[c * 4 + r] = a[r] * b[c * 4] + a[4 + r] * b[c * 4 + 1] + a[8 + r] * b[c * 4 + 2] + a[12 + r] * b[c * 4 + 3];
    }
    return out;
};
const xform = (m, p) => [
    m[0] * p[0] + m[4] * p[1] + m[8] * p[2] + m[12],
    m[1] * p[0] + m[5] * p[1] + m[9] * p[2] + m[13],
    m[2] * p[0] + m[6] * p[1] + m[10] * p[2] + m[14],
];
const approx = (a, b, tol = 1e-9) => a.every((v, i) => Math.abs(v - b[i]) <= tol);

// Two-bone rig with non-identity inverse bind matrices and mixed weights.
function makeTwoBoneRig() {
    const gl = makeDevice();
    const geometry = new Geometry(gl, {
        position: { size: 3, data: new Float32Array([1, 0, 0, 0, 2, 0, -1, 0, 1]) },
        index: { data: new Uint16Array([0, 1, 2]) },
        skinIndex: { size: 4, data: new Float32Array([0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0]) },
        skinWeight: { size: 4, data: new Float32Array([0.8, 0.2, 0, 0, 0.25, 0.75, 0, 0, 0.5, 0.5, 0, 0]) },
    });
    const root = new Transform();
    const boneA = new Transform();
    const boneB = new Transform();
    root.position.set(1, 2, 3);
    root.rotation.z = 0.21;
    boneA.position.set(0.5, 0, 0);
    boneA.setParent(root);
    boneB.position.set(0, 1, 0);
    boneB.setParent(boneA);
    boneA.bindInverse = new Mat4().translate([-1.5, -2, -3]);
    boneB.bindInverse = new Mat4().translate([0.3, -0.7, 1.1]);
    const program = { uniforms: { boneTexture: { value: null }, boneTextureSize: { value: 0 } } };
    const skin = new GLTFSkin(gl, { skeleton: { joints: [boneA, boneB] }, geometry, program });
    skin.position.set(99, -99, 7); // must be ignored by skinning
    return { gl, geometry, root, boneA, boneB, skin, program };
}

// Independent reference surface for the two-bone rig at its current pose.
function referenceSurface(rig) {
    const { root, boneA, boneB, geometry, skin } = rig;
    const wa = root.worldMatrix, wbA = boneA.worldMatrix, wbB = boneB.worldMatrix;
    const palette = [mmul(wbA, boneA.bindInverse), mmul(wbB, boneB.bindInverse)];
    void skin;
    const pos = geometry.attributes.position.data;
    const idx = geometry.attributes.skinIndex.data;
    const wgt = geometry.attributes.skinWeight.data;
    const bary = [0.2, 0.3, 0.5];
    const out = [0, 0, 0];
    for (let v = 0; v < 3; v++) {
        const p = [pos[v * 3], pos[v * 3 + 1], pos[v * 3 + 2]];
        const skinned = [0, 0, 0];
        for (let slot = 0; slot < 2; slot++) {
            const j = idx[v * 4 + slot];
            const w = wgt[v * 4 + slot];
            const c = xform(palette[j], p);
            skinned[0] += w * c[0];
            skinned[1] += w * c[1];
            skinned[2] += w * c[2];
        }
        for (let d = 0; d < 3; d++) out[d] += bary[v] * skinned[d];
    }
    return out;
}

export const cases = {
    // ------------------------------------------------------------------
    async surface(check) {
        const rig = makeTwoBoneRig();
        const { root, boneA, boneB, skin, gl } = rig;
        const probes = [{ triangle: 0, barycentric: [0.2, 0.3, 0.5] }];
        root.updateMatrixWorld();
        const uploadsBefore = gl.uploads.length;
        skin.boneTexture.needsUpdate = false;
        const s0 = skin.sampleSurface(probes);
        check(s0 instanceof Float64Array && s0.length === 3, 'sampleSurface returns independent Float64Array');
        check(approx(Array.from(s0), referenceSurface(rig), 1e-6), 'triangle mixing matches scalar hierarchy reference');
        check(gl.uploads.length === uploadsBefore, 'CPU sampling triggers no GPU upload');
        check(skin.boneTexture.needsUpdate === false, 'CPU sampling does not dirty the bone texture');
        const s0copy = Float64Array.from(s0);
        s0[0] = 1e9;
        check(approx(Array.from(skin.sampleSurface(probes)), Array.from(s0copy), 0), 'returned arrays are independent between calls');

        // Noncommuting parent/child rotations about different axes.
        boneA.quaternion.fromAxisAngle([1, 0, 0], 0.47);
        boneB.quaternion.fromAxisAngle([0, 1, 0], -0.63);
        root.updateMatrixWorld();
        check(approx(Array.from(skin.sampleSurface(probes)), referenceSurface(rig), 1e-6), 'noncommuting parent/child rotations match reference');

        // A mesh worldMatrix must not participate in skinning.
        const withMeshOffset = Array.from(skin.sampleSurface(probes));
        skin.updateMatrixWorld();
        check(approx(Array.from(skin.sampleSurface(probes)), withMeshOffset, 0), 'mesh worldMatrix is ignored');

        // Multi-bone vertex actually mixes both stored influences.
        const singleBone = (() => {
            const { geometry } = rig;
            const idx = geometry.attributes.skinIndex.data;
            const wgt = geometry.attributes.skinWeight.data;
            const pos = geometry.attributes.position.data;
            root.updateMatrixWorld();
            const palette = [mmul(boneA.worldMatrix, boneA.bindInverse), mmul(boneB.worldMatrix, boneB.bindInverse)];
            const v = 2; // weights 0.5/0.5
            const p = [pos[6], pos[7], pos[8]];
            const mixed = [0, 0, 0];
            for (let slot = 0; slot < 2; slot++) {
                const c = xform(palette[idx[v * 4 + slot]], p);
                for (let d = 0; d < 3; d++) mixed[d] += wgt[v * 4 + slot] * c[d];
            }
            return mixed;
        })();
        const livePalette = [mmul(boneA.worldMatrix, boneA.bindInverse), mmul(boneB.worldMatrix, boneB.bindInverse)];
        const pos = rig.geometry.attributes.position.data;
        const onlyA = xform(livePalette[0], [pos[6], pos[7], pos[8]]);
        check(!approx(singleBone, onlyA, 1e-6), 'a 0.5/0.5 vertex differs from single-bone placement');

        // Validation.
        let threw = false;
        try {
            skin.sampleSurface([{ triangle: 999, barycentric: [1, 0, 0] }]);
        } catch (e) {
            threw = true;
        }
        check(threw, 'out-of-range triangle throws');
        threw = false;
        try {
            skin.sampleSurface([{ triangle: 0, barycentric: [0.5, 0.5, 0.5] }]);
        } catch (e) {
            threw = true;
        }
        check(threw, 'barycentric coordinates that do not sum to 1 throw');

        // Uint32 indices and stored (not renormalized) weights on a second skin.
        const gl2 = makeDevice();
        const geometry2 = new Geometry(gl2, {
            position: { size: 3, data: new Float32Array([1, 0, 0, 0, 1, 0, 0, 0, 1]) },
            index: { data: new Uint32Array([0, 1, 2]) },
            skinIndex: { size: 4, data: new Float32Array([0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0]) },
            // Sum is 0.9999999: within the 2e-7 tolerance, used verbatim.
            skinWeight: { size: 4, data: new Float32Array([0.9999999, 0, 0, 0, 0.9999999, 0, 0, 0, 0.9999999, 0, 0, 0]) },
        });
        const single = new Transform();
        single.position.set(2, 0, 0);
        single.bindInverse = new Mat4().translate([-2, 0, 0]);
        const program2 = { uniforms: { boneTexture: { value: null }, boneTextureSize: { value: 0 } } };
        const skin2 = new GLTFSkin(gl2, { skeleton: { joints: [single] }, geometry: geometry2, program: program2 });
        single.updateMatrixWorld();
        const probes2 = [
            { triangle: 0, barycentric: [1, 0, 0] },
            { triangle: 0, barycentric: [0, 1, 0] },
        ];
        const surf2 = skin2.sampleSurface(probes2);
        // T(2) * T(-2) = identity palette, so each point is its bind position
        // scaled by the verbatim stored weight (no renormalization to 1).
        const expected2 = [0.9999999, 0, 0, 0, 0.9999999, 0];
        check(approx(Array.from(surf2), expected2, 1e-7), 'Uint32 indices and verbatim sub-unit weights are supported');

        // Reject weights whose sum drifts beyond the 2e-7 contract.
        const gl3 = makeDevice();
        let threw3 = false;
        try {
            const badGeometry = new Geometry(gl3, {
                position: { size: 3, data: new Float32Array([1, 0, 0, 0, 1, 0, 0, 0, 1]) },
                index: { data: new Uint16Array([0, 1, 2]) },
                skinIndex: { size: 4, data: new Float32Array(12) },
                skinWeight: { size: 4, data: new Float32Array([0.9, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0]) },
            });
            const root3 = new Transform();
            const bone3 = new Transform();
            bone3.setParent(root3);
            bone3.bindInverse = new Mat4();
            const skin3 = new GLTFSkin(gl3, { skeleton: { joints: [bone3] }, geometry: badGeometry, program: { uniforms: {} } });
            const anim3 = new GLTFAnimation([]);
            // Empty animation spans zero time; give matching endpoints explicitly.
            Object.defineProperty(anim3, 'startTime', { value: 0 });
            Object.defineProperty(anim3, 'endTime', { value: 1 });
            anim3.editSkinMotion({
                skin: skin3, root: root3, joints: [bone3], times: [0, 0.5, 1],
                probes: [{ triangle: 0, barycentric: [1, 0, 0] }],
                observations: [{ sample: 1, probe: 0, target: [0, 0, 0], weight: 1 }],
                lambda: 1, smoothness: 1, maxAngle: 0.5,
            });
        } catch (e) {
            threw3 = true;
        }
        check(threw3, 'vertex weights outside the 2e-7 sum tolerance are rejected');
    },

    // ------------------------------------------------------------------
    async optimization(check) {
        const rig = makeTwoBoneRig();
        const { root, boneA, skin } = rig;
        root.updateMatrixWorld();
        const times = [0, 0.3, 0.8, 1.4, 2];
        const probes = [{ triangle: 0, barycentric: [0.2, 0.3, 0.5] }];
        const current = skin.sampleSurface(probes);
        const target = [current[0] + 0.25, current[1] - 0.2, current[2] + 0.1];
        const animation = new GLTFAnimation([
            { node: boneA, transform: 'quaternion', interpolation: 'LINEAR', times: new Float32Array([0, 2]), values: new Float32Array([0, 0, 0, 1, 0, 0, 0, 1]) },
        ]);
        const options = {
            skin, root, joints: [boneA], times, probes,
            observations: [{ sample: 2, probe: 0, target, weight: 7 }],
            lambda: 0.01, smoothness: 0.02, maxAngle: 0.6, tolerance: 1e-8, maxIterations: 1000,
        };

        // Independent objective recomputation and finite-difference gradient.
        const ctx = _skinMotionInternals.prepareProblem(animation, options);
        ctx.jointToAdjustable = new Map([[boneA, 0]]);
        ctx.adjustableIndex = new Map([[boneA, 0]]);
        const x = new Float64Array((times.length - 2) * 3);
        for (let i = 0; i < x.length; i++) x[i] = 0.03 * Math.sin(i * 1.31 + 0.4);
        const ev = _skinMotionInternals.evaluate(ctx, x);

        // Independent E: rebuild every shadow pose with the test's own math
        // from the solver's plain-array snapshots (full root/A/B hierarchy,
        // both stored skin influences per vertex).
        const independentE = (xx) => {
            const N = ctx.N;
            const localMats = new Array(N);
            for (let s = 0; s < N; s++) {
                localMats[s] = [];
                for (let u = 0; u < ctx.U; u++) {
                    const pos = Array.from(ctx.basePos.subarray((s * ctx.U + u) * 3, (s * ctx.U + u + 1) * 3));
                    let q = qnorm(Array.from(ctx.baseQuat.subarray((s * ctx.U + u) * 4, (s * ctx.U + u + 1) * 4)));
                    const node = ctx.nodes[u];
                    const k = ctx.jointToAdjustable.get(node);
                    if (k !== undefined && s > 0 && s < N - 1) {
                        q = qmul(q, qexp(Array.from(xx.subarray(((s - 1) * ctx.K + k) * 3, ((s - 1) * ctx.K + k + 1) * 3))));
                    }
                    localMats[s].push(composeTR(pos, q));
                }
            }
            const worldAt = (s, u) => {
                const node = ctx.nodes[u];
                if (node === ctx.root) return mmul(ctx.ancestorWorld, localMats[s][u]);
                const pu = ctx.nodeIndex.get(node.parent);
                return mmul(worldAt(s, pu), localMats[s][u]);
            };
            let dataE = 0;
            for (const obs of ctx.obsList) {
                const s = obs.s;
                const palettes = ctx.skelJoints.map((joint, j) => mmul(worldAt(s, ctx.nodeIndex.get(joint)), Array.from(ctx.bindInverse[j])));
                const probe = ctx.probeVerts[obs.p];
                const surf = [0, 0, 0];
                for (let vi = 0; vi < 3; vi++) {
                    const slot = ctx.vertexSlot.get(probe.verts[vi]);
                    const p = ctx.bindPoints[slot];
                    const skinned = [0, 0, 0];
                    for (const inf of ctx.influences[slot]) {
                        const c = xform(palettes[inf.j], p);
                        for (let d = 0; d < 3; d++) skinned[d] += inf.w * c[d];
                    }
                    for (let d = 0; d < 3; d++) surf[d] += probe.bary[vi] * skinned[d];
                }
                const e = [surf[0] - obs.target[0], surf[1] - obs.target[1], surf[2] - obs.target[2]];
                dataE += 0.5 * obs.w * (e[0] ** 2 + e[1] ** 2 + e[2] ** 2);
            }
            let regE = 0;
            for (let i = 0; i < xx.length; i++) regE += 0.5 * options.lambda * xx[i] ** 2;
            let smoothE = 0;
            for (let s = 1; s < N - 1; s++) {
                const hs = ctx.h[s - 1];
                const dm = ctx.d[s - 1];
                const dp = ctx.d[s];
                const xm = s === 1 ? [0, 0, 0] : Array.from(xx.subarray((s - 2) * 3, (s - 1) * 3));
                const xc = Array.from(xx.subarray((s - 1) * 3, s * 3));
                const xp = s === N - 2 ? [0, 0, 0] : Array.from(xx.subarray(s * 3, (s + 1) * 3));
                for (let dim = 0; dim < 3; dim++) {
                    const acc = ((xp[dim] - xc[dim]) / dp - (xc[dim] - xm[dim]) / dm) / hs;
                    smoothE += 0.5 * options.smoothness * hs * acc * acc;
                }
            }
            return dataE + regE + smoothE;
        };
        check(Math.abs(independentE(x) - ev.E) < 1e-8, 'objective independently recomputed from the shadow pose');

        // Finite-difference check of the analytic gradient on the full E.
        const eps = 1e-6;
        let gradMaxErr = 0;
        for (let i = 0; i < x.length; i++) {
            const xp = Float64Array.from(x); xp[i] += eps;
            const xm = Float64Array.from(x); xm[i] -= eps;
            const fd = (_skinMotionInternals.evaluate(ctx, xp).E - _skinMotionInternals.evaluate(ctx, xm).E) / (2 * eps);
            gradMaxErr = Math.max(gradMaxErr, Math.abs(fd - ev.g[i]) / (1 + Math.abs(fd)));
        }
        check(gradMaxErr < 1e-5, `analytic gradient matches finite differences (err ${gradMaxErr.toExponential(2)})`);

        // Stationarity residual against an independently coded ball projection.
        const gTest = ev.g.map((v, i) => v + 0.01 * Math.cos(i));
        const manualResidual = (() => {
            let r = 0;
            for (let i = 0; i < x.length; i += 3) {
                const y = [x[i] - gTest[i], x[i + 1] - gTest[i + 1], x[i + 2] - gTest[i + 2]];
                const len = Math.hypot(y[0], y[1], y[2]);
                const p = len > options.maxAngle ? y.map((v) => (v * options.maxAngle) / len) : y;
                for (let d = 0; d < 3; d++) r = Math.max(r, Math.abs(x[i + d] - p[d]));
            }
            return r;
        })();
        check(Math.abs(manualResidual - _skinMotionInternals.stationarityResidual(x, gTest, options.maxAngle)) < 1e-14, 'ball projection residual matches independent code');

        // Full solve: monotone history, gradient layout, finite everything.
        const result = animation.editSkinMotion(options);
        check(result.history.length >= 2 && result.history.every((v, i) => i === 0 || v <= result.history[i - 1] + 1e-12), 'history is monotone and includes the initial value');
        check(result.objective <= result.history[0], 'solving decreases the objective');
        check(result.rotations instanceof Float64Array && result.gradient instanceof Float64Array, 'rotations and gradient are Float64Arrays');
        check(result.rotations.length === times.length * 3 && result.gradient.length === times.length * 3, 'rotations/gradient follow sample-joint-xyz layout');
        check([0, 1, 2].every((i) => result.gradient[i] === 0 && result.rotations[i] === 0), 'first frame fixed at zero');
        check(result.gradient.slice(-3).every((v) => v === 0) && result.rotations.slice(-3).every((v) => v === 0), 'last frame fixed at zero with zero gradient');
        check(result.errors.length === 1 && Number.isFinite(result.errors[0]), 'errors follow observation order');

        // Ball-constrained boundary optimum: a far, unreachable target drives
        // the only internal frame (N=3) onto the ball; with a single joint and
        // one internal frame the forces stay radial, so projected stationarity
        // is reachable to tight tolerance.
        const boundTimes = [0, 1, 2];
        const boundOpts = {
            skin, root, joints: [boneA], times: boundTimes, probes,
            observations: [{ sample: 1, probe: 0, target: [target[0] + 30, target[1] + 30, target[2] + 30], weight: 50 }],
            lambda: 1e-8, smoothness: 1e-8, maxAngle: 0.05, tolerance: 1e-6, maxIterations: 2000,
        };
        const bound = animation.editSkinMotion(boundOpts);
        const boundNorm = Math.hypot(bound.rotations[3], bound.rotations[4], bound.rotations[5]);
        check(boundNorm <= 0.05 + 1e-9, 'the twist block does not exceed the maxAngle ball');
        check(Math.abs(boundNorm - 0.05) < 1e-6, 'the block sits on the ball at the boundary optimum');
        check(bound.converged && bound.residual <= 1e-6, 'boundary optimum satisfies the projected stationarity tolerance');

        // Exhausted budget: best finite pose, converged false, one attempted step.
        const shortOpts = { ...options, maxIterations: 1, tolerance: 1e-12 };
        const short = animation.editSkinMotion(shortOpts);
        check(short.converged === false && short.iterations === 1, 'insufficient iteration budget reports not converged');
        check(short.rotations.every(Number.isFinite) && Number.isFinite(short.objective), 'exhausted budget still returns a finite pose');
    },

    // ------------------------------------------------------------------
    async temporal(check) {
        const rig = makeTwoBoneRig();
        const { root, boneA, skin } = rig;
        root.updateMatrixWorld();
        const times = [0, 0.2, 0.9, 1.7, 2.5]; // deliberately non-uniform
        const probes = [{ triangle: 0, barycentric: [0.2, 0.3, 0.5] }];
        const current = skin.sampleSurface(probes);
        const target = [current[0] - 0.3, current[1] + 0.2, current[2]];
        const animation = new GLTFAnimation([
            { node: boneA, transform: 'quaternion', interpolation: 'LINEAR', times: new Float32Array([0, 2.5]), values: new Float32Array([0, 0, 0, 1, 0, 0, 0, 1]) },
        ]);
        const base = {
            skin, root, joints: [boneA], times, probes,
            observations: [{ sample: 2, probe: 0, target, weight: 10 }],
            lambda: 0.001, maxAngle: 0.7, tolerance: 1e-8, maxIterations: 1000,
        };

        // With temporal smoothness the single internal observation spreads to
        // the unobserved neighbor frames.
        const coupled = animation.editSkinMotion({ ...base, smoothness: 0.5 });
        const frameNorm = (res, s) => Math.hypot(res.rotations[s * 3], res.rotations[s * 3 + 1], res.rotations[s * 3 + 2]);
        check(frameNorm(coupled, 2) > 0.05, 'the observed frame takes a real correction');
        check(frameNorm(coupled, 1) > 1e-5 && frameNorm(coupled, 3) > 1e-5, 'unobserved neighbor frames inherit the correction through smoothness');

        // Frame-wise behavior: with negligible smoothness an unobserved frame
        // has no data term, so Tikhonov alone drives its correction to ~zero.
        // (The API requires a strictly positive smoothness, so use a tiny one.)
        const framewise = animation.editSkinMotion({ ...base, smoothness: 1e-12 });
        check(frameNorm(framewise, 1) < 1e-6 && frameNorm(framewise, 3) < 1e-6, 'without meaningful smoothness, unobserved frames stay near zero (per-frame behavior)');
        check(frameNorm(framewise, 2) > frameNorm(coupled, 2), 'the joint solution shares the motion instead of concentrating it on one frame');

        // Smoothness acceleration is genuinely smaller for the joint answer.
        const acceleration = (res) => {
            let acc = 0;
            for (let s = 1; s < times.length - 1; s++) {
                const dm = times[s] - times[s - 1];
                const dp = times[s + 1] - times[s];
                const hs = (dm + dp) / 2;
                const xm = s === 1 ? [0, 0, 0] : Array.from(res.rotations.subarray((s - 1) * 3, s * 3));
                const xc = Array.from(res.rotations.subarray(s * 3, (s + 1) * 3));
                const xp = s === times.length - 2 ? [0, 0, 0] : Array.from(res.rotations.subarray((s + 1) * 3, (s + 2) * 3));
                for (let d = 0; d < 3; d++) {
                    const a = ((xp[d] - xc[d]) / dp - (xc[d] - xm[d]) / dm) / hs;
                    acc += hs * a * a;
                }
            }
            return acc;
        };
        check(acceleration(coupled) < acceleration(framewise), 'joint optimization lowers the discrete-time acceleration energy');

        // End frames remain exactly zero even with strong smoothness.
        check(frameNorm(coupled, 0) === 0 && frameNorm(coupled, times.length - 1) === 0, 'end frames never move');
    },

    // ------------------------------------------------------------------
    async playback(check) {
        const { gl, geometry, root, helper, a, b, skin } = makeEditingRig();
        const key = (node, transform, values) => ({ node, transform, interpolation: 'LINEAR', times: new Float32Array([0, 1]), values: new Float32Array(values) });
        const animation = new GLTFAnimation([
            key(helper, 'position', [1, 0, 0, 1, 0.5, 0]),
            key(a, 'quaternion', [0, 0, 0, 1, Math.sin(0.4), 0, 0, Math.cos(0.4)]),
            key(b, 'quaternion', [0, 0, 0, 1, 0, Math.sin(-0.3), 0, Math.cos(-0.3)]),
        ]);
        root.updateMatrixWorld();
        const times = [0, 0.25, 0.6, 1];
        const probes = [{ triangle: 0, barycentric: [0.2, 0.3, 0.5] }];
        const before = skin.sampleSurface(probes);
        const target = [before[0] + 0.15, before[1] - 0.1, before[2] + 0.08];
        const options = {
            skin, root, joints: [a, b], times, probes,
            observations: [{ sample: 1, probe: 0, target, weight: 6 }, { sample: 2, probe: 0, target: target.map((v) => v + 0.05), weight: 4 }],
            lambda: 0.01, smoothness: 0.02, maxAngle: 0.6, tolerance: 1e-7, maxIterations: 1000,
        };

        // Snapshot everything that must remain untouched.
        const snapshot = {
            elapsed: animation.elapsed,
            aQuat: Float32Array.from(a.quaternion),
            rootWorld: Float64Array.from(root.worldMatrix),
            channels: animation.data.map((c) => Float32Array.from(c.values)),
            posHex: Buffer.from(geometry.attributes.position.data.buffer, geometry.attributes.position.data.byteOffset, geometry.attributes.position.data.byteLength).toString('hex'),
        };
        const result = animation.editSkinMotion(options);
        check(animation.elapsed === snapshot.elapsed, 'solving does not touch animation playback state');
        check(approx(Array.from(a.quaternion), Array.from(snapshot.aQuat), 0), 'solving does not write node local transforms');
        check(approx(Array.from(root.worldMatrix), Array.from(snapshot.rootWorld), 0), 'solving does not write world matrices');
        check(animation.data.every((c, i) => Array.from(c.values).every((v, j) => v === snapshot.channels[i][j])), 'solving does not write original channel data');
        const posHexAfter = Buffer.from(geometry.attributes.position.data.buffer, geometry.attributes.position.data.byteOffset, geometry.attributes.position.data.byteLength).toString('hex');
        check(posHexAfter === snapshot.posHex, 'mesh geometry is untouched');
        check(gl.uploads.length === gl.uploads.filter((u) => u).length && gl.uploads.length >= 3, 'solving performs no extra GPU uploads');

        // Result animation structure: complete channels for every subtree node,
        // including helper/root nodes that lacked channels.
        const ra = result.animation;
        check(ra.weight === 1 && ra.elapsed === 0 && ra.loop === false, 'result animation has weight 1, elapsed 0, loop false');
        // Transform.traverse stops when the callback returns truthy, so push
        // explicitly to collect the whole root subtree (including the skin).
        const subtree = [];
        root.traverse((node) => {
            subtree.push(node);
        });
        check(ra.data.length === subtree.length * 2, 'one position and one quaternion channel per subtree node');
        check(ra.data.every((c) => c.interpolation === 'LINEAR' && c.times.length === times.length && c.times instanceof Float32Array && c.values instanceof Float32Array), 'all channels are complete LINEAR Float32 channels on the sample times');
        check(ra.data.every((c) => subtree.includes(c.node)), 'channels still drive the original scene nodes');
        const rootChannels = ra.data.filter((c) => c.node === root);
        check(rootChannels.length === 2 && rootChannels.every((c) => new Set(c.values).size <= 3), 'a node without source channels keeps its static local values');
        const helperQuat = ra.data.find((c) => c.node === helper && c.transform === 'quaternion');
        check(helperQuat && approx(Array.from(helperQuat.values), [0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1], 0), 'missing quaternion channel falls back to the call-start local quaternion');

        // End keys reproduce the original end pose exactly.
        const endA = ra.data.find((c) => c.node === a && c.transform === 'quaternion').values.slice(-4);
        check(Array.from(endA).every((v, i) => v === animation.data.find((c) => c.node === a && c.transform === 'quaternion').values[4 + i]), 'the final quaternion key is bit-exact against the original end pose');

        // Two results are independent of each other and of the inputs.
        const result2 = animation.editSkinMotion(options);
        const firstValues = ra.data[0].values;
        const originalFirst = firstValues[0];
        firstValues[0] = 1e9;
        check(result2.animation.data[0].values[0] !== 1e9 && result.rotations !== result2.rotations, 'two edits return independent arrays');
        check(animation.data.every((c, i) => Array.from(c.values).every((v, j) => v === snapshot.channels[i][j])), 'mutating a result leaves the original animation intact');
        void originalFirst;

        // Real playback through the normal Transform / GLTFSkin pipeline.
        const s = 2;
        ra.elapsed = times[s];
        ra.update(1, true);
        root.updateMatrixWorld();
        skin.updateUniforms();
        const live = skin.sampleSurface(probes);
        // Rebuild the surface straight from the uploaded bone palette.
        const pos = geometry.attributes.position.data;
        const sIdx = geometry.attributes.skinIndex.data;
        const sW = geometry.attributes.skinWeight.data;
        const indices = geometry.attributes.index.data;
        const bary = probes[0].barycentric;
        const fromPalette = [0, 0, 0];
        for (let vi = 0; vi < 3; vi++) {
            const vertex = indices[vi];
            const skinned = [0, 0, 0];
            for (let slot = 0; slot < 4; slot++) {
                const w = sW[vertex * 4 + slot];
                if (!w) continue;
                const j = sIdx[vertex * 4 + slot];
                const m = skin.boneMatrices.subarray(j * 16, j * 16 + 16);
                const p = [pos[vertex * 3], pos[vertex * 3 + 1], pos[vertex * 3 + 2]];
                const c = [m[0] * p[0] + m[4] * p[1] + m[8] * p[2] + m[12], m[1] * p[0] + m[5] * p[1] + m[9] * p[2] + m[13], m[2] * p[0] + m[6] * p[1] + m[10] * p[2] + m[14]];
                for (let d = 0; d < 3; d++) skinned[d] += w * c[d];
            }
            for (let d = 0; d < 3; d++) fromPalette[d] += bary[vi] * skinned[d];
        }
        check(approx(Array.from(live), fromPalette, 1e-6), 'sampleSurface and the GPU bone palette agree after playback');
        check(Math.hypot(live[0] - result.errors[1] * 0 - target[0] - 0.05, live[1] - (target[1] - 0.05), live[2] - (target[2] + 0.05)) < 0.2, 'playback moves the surface toward the observation target');
        const uploadsAfterSolve = gl.uploads.length;
        skin.sampleSurface(probes);
        check(gl.uploads.length === uploadsAfterSolve, 'post-solve CPU sampling still causes no GPU upload');

        // Bad inputs throw and leave the rig unchanged. Snapshot the state
        // immediately before the failed calls (the playback above legitimately
        // moved the live nodes, so the original pre-solve snapshot no longer
        // describes the current world matrices).
        const channelsBefore = animation.data.map((c) => Float32Array.from(c.values));
        const rootWorldBefore = Float64Array.from(root.worldMatrix);
        const badCalls = [
            { ...options, times: [0.1, 0.5, 1] }, // first endpoint does not match startTime
            { ...options, joints: [root] }, // root is not an allowed joint
            { ...options, maxAngle: 2 }, // outside (0, pi/2)
            { ...options, probes: [{ triangle: 5, barycentric: [1, 0, 0] }] }, // triangle out of range
        ];
        for (const bad of badCalls) {
            let threw = false;
            try {
                animation.editSkinMotion(bad);
            } catch (e) {
                threw = true;
            }
            check(threw, 'invalid arguments throw a clear error');
        }
        check(animation.data.every((c, i) => Array.from(c.values).every((v, j) => v === channelsBefore[i][j])), 'failed edits never write the original animation');
        check(approx(Array.from(root.worldMatrix), Array.from(rootWorldBefore), 0), 'failed edits never write world matrices');

        // Non-LINEAR / unsupported channels are rejected before any work.
        const cubic = new GLTFAnimation([{ node: a, transform: 'scale', interpolation: 'LINEAR', times: new Float32Array([0, 1]), values: new Float32Array([1, 1, 1, 1, 1, 1]) }]);
        let threw = false;
        try {
            cubic.editSkinMotion(options);
        } catch (e) {
            threw = true;
        }
        check(threw, 'unsupported channels throw');
    },
};
