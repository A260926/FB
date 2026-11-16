import { Mesh } from '../core/Mesh.js';
import { Mat4 } from '../math/Mat4.js';
import { Texture } from '../core/Texture.js';

const tempMat4 = /* @__PURE__ */ new Mat4();
const identity = /* @__PURE__ */ new Mat4();

// Production palette assembly for one joint: worldMatrix * bindInverse.
// Shared by the GPU bone texture update, CPU surface sampling and the motion
// editing solver so every consumer agrees on the binding convention. `out` may
// be any 16-component array (Mat4, Float32Array or Float64Array).
export function assembleBoneMatrix(skin, jointIndex, out) {
    const bone = skin.skeleton.joints[jointIndex];
    if (!out || out === tempMat4) {
        tempMat4.multiply(bone.worldMatrix, bone.bindInverse);
        return tempMat4;
    }
    tempMat4.multiply(bone.worldMatrix, bone.bindInverse);
    for (let i = 0; i < 16; i++) out[i] = tempMat4[i];
    return out;
}

function finiteNumber(v) {
    return typeof v === 'number' && Number.isFinite(v);
}

export class GLTFSkin extends Mesh {
    constructor(gl, { skeleton, geometry, program, mode = gl.TRIANGLES } = {}) {
        super(gl, { geometry, program, mode });
        this.skeleton = skeleton;
        this.program = program;
        this.createBoneTexture();
    }

    createBoneTexture() {
        if (!this.skeleton.joints.length) return;
        const size = Math.max(4, Math.pow(2, Math.ceil(Math.log(Math.sqrt(this.skeleton.joints.length * 4)) / Math.LN2)));
        this.boneMatrices = new Float32Array(size * size * 4);
        this.boneTextureSize = size;
        this.boneTexture = new Texture(this.gl, {
            image: this.boneMatrices,
            generateMipmaps: false,
            type: this.gl.FLOAT,
            internalFormat: this.gl.renderer.isWebgl2 ? this.gl.RGBA32F : this.gl.RGBA,
            minFilter: this.gl.NEAREST,
            magFilter: this.gl.NEAREST,
            flipY: false,
            width: size,
        });
    }

    updateUniforms() {
        // Update bone texture
        this.skeleton.joints.forEach((bone, i) => {
            // Find difference between current and bind pose
            assembleBoneMatrix(this, i, tempMat4);
            this.boneMatrices.set(tempMat4, i * 16);
        });
        this.boneTexture.needsUpdate = true;
        // Reset for programs shared between multiple skins
        this.program.uniforms.boneTexture.value = this.boneTexture;
        this.program.uniforms.boneTextureSize.value = this.boneTextureSize;
    }

    // CPU surface points for the current joint world matrices. Callers must
    // have run updateMatrixWorld themselves; this never uploads to the GPU and
    // never marks the bone texture dirty. Each probe is {triangle,
    // barycentric}: the three bound vertices are skinned independently with
    // their stored skinIndex/skinWeight (no renormalization, mesh worldMatrix
    // ignored just like in draw) and only afterwards mixed barycentrically.
    sampleSurface(probes) {
        if (!Array.isArray(probes) || !probes.length) throw new Error('sampleSurface: probes must be a non-empty array');
        const geometry = this.geometry;
        const indexAttr = geometry.attributes.index;
        const indexData = indexAttr.data;
        if (!(indexData instanceof Uint16Array || indexData instanceof Uint32Array)) {
            throw new Error('sampleSurface: index must be a Uint16Array or Uint32Array');
        }
        const { position, skinIndex, skinWeight } = geometry.attributes;
        if (!(position.data instanceof Float32Array) || position.size !== 3) {
            throw new Error('sampleSurface: position must be a tight Float32 attribute of size 3');
        }
        if (!(skinIndex.data instanceof Float32Array) || skinIndex.size !== 4 || !(skinWeight.data instanceof Float32Array) || skinWeight.size !== 4) {
            throw new Error('sampleSurface: skinIndex/skinWeight must be Float32 attributes of size 4');
        }
        const joints = this.skeleton.joints;
        const jointCount = joints.length;
        // One palette matrix per joint, assembled with the production helper.
        const palette = new Array(jointCount);
        for (let j = 0; j < jointCount; j++) {
            palette[j] = new Float64Array(16);
            assembleBoneMatrix(this, j, palette[j]);
        }

        const positionData = position.data;
        const jointData = skinIndex.data;
        const weightData = skinWeight.data;
        const result = new Float64Array(probes.length * 3);
        const transformed = [0, 0, 0];
        const skinnedPoints = [
            [0, 0, 0],
            [0, 0, 0],
            [0, 0, 0],
        ];

        const skinVertex = (vertexIndex, out) => {
            const p0 = positionData[vertexIndex * 3];
            const p1 = positionData[vertexIndex * 3 + 1];
            const p2 = positionData[vertexIndex * 3 + 2];
            out[0] = 0;
            out[1] = 0;
            out[2] = 0;
            for (let slot = 0; slot < 4; slot++) {
                const w = weightData[vertexIndex * 4 + slot];
                if (!w) continue;
                const j = jointData[vertexIndex * 4 + slot];
                if (j < 0 || j >= jointCount) throw new Error(`sampleSurface: skinIndex ${j} out of range`);
                const m = palette[j];
                transformed[0] = m[0] * p0 + m[4] * p1 + m[8] * p2 + m[12];
                transformed[1] = m[1] * p0 + m[5] * p1 + m[9] * p2 + m[13];
                transformed[2] = m[2] * p0 + m[6] * p1 + m[10] * p2 + m[14];
                out[0] += w * transformed[0];
                out[1] += w * transformed[1];
                out[2] += w * transformed[2];
            }
        };

        probes.forEach((probe, pi) => {
            if (!probe || !Number.isInteger(probe.triangle) || probe.triangle < 0 || probe.triangle * 3 + 2 >= indexData.length) {
                throw new Error(`sampleSurface: probe ${pi} triangle out of range`);
            }
            const bary = probe.barycentric;
            if (!bary || bary.length !== 3) throw new Error(`sampleSurface: probe ${pi} barycentric must have three components`);
            let bSum = 0;
            for (let i = 0; i < 3; i++) {
                if (!finiteNumber(bary[i]) || bary[i] < 0) throw new Error(`sampleSurface: probe ${pi} barycentric weights must be finite and non-negative`);
                bSum += bary[i];
            }
            if (Math.abs(bSum - 1) > 1e-9) throw new Error(`sampleSurface: probe ${pi} barycentric weights must sum to 1`);

            const first = probe.triangle * 3;
            const verts = [indexData[first], indexData[first + 1], indexData[first + 2]];
            for (let i = 0; i < 3; i++) skinVertex(verts[i], skinnedPoints[i]);

            const o = pi * 3;
            for (let d = 0; d < 3; d++) {
                result[o + d] = bary[0] * skinnedPoints[0][d] + bary[1] * skinnedPoints[1][d] + bary[2] * skinnedPoints[2][d];
            }
        });

        return result;
    }

    draw({ camera } = {}) {
        if (!this.program.uniforms.boneTexture) {
            Object.assign(this.program.uniforms, {
                boneTexture: { value: this.boneTexture },
                boneTextureSize: { value: this.boneTextureSize },
            });
        }

        this.updateUniforms();

        // Switch the world matrix with identity to ignore any transforms
        // on the mesh itself - only use skeleton's transforms
        const _worldMatrix = this.worldMatrix;
        this.worldMatrix = identity;

        super.draw({ camera });

        // Switch back to leave identity untouched
        this.worldMatrix = _worldMatrix;
    }
}
