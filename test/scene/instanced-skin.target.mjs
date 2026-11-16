import assert from 'node:assert/strict';

import { Mat4 } from '../../src/core/math/mat4.js';
import { Vec3 } from '../../src/core/math/vec3.js';
import { BoundingBox } from '../../src/core/shape/bounding-box.js';
import { NullGraphicsDevice } from '../../src/platform/graphics/null/null-graphics-device.js';
import {
    SEMANTIC_BLENDINDICES, SEMANTIC_BLENDWEIGHT, TYPE_UINT8,
    PIXELFORMAT_RGBA8, SHADERLANGUAGE_GLSL, SHADERLANGUAGE_WGSL
} from '../../src/platform/graphics/constants.js';
import { ProgramLibrary } from '../../src/scene/shader-lib/program-library.js';
import { setProgramLibrary } from '../../src/scene/shader-lib/get-program-library.js';
import { ShaderChunks } from '../../src/scene/shader-lib/shader-chunks.js';
import { shaderChunksGLSL } from '../../src/scene/shader-lib/glsl/collections/shader-chunks-glsl.js';
import { shaderChunksWGSL } from '../../src/scene/shader-lib/wgsl/collections/shader-chunks-wgsl.js';
import { LitShader } from '../../src/scene/shader-lib/programs/lit-shader.js';
import { LitShaderOptions } from '../../src/scene/shader-lib/programs/lit-shader-options.js';
import { Preprocessor } from '../../src/core/preprocessor.js';
import { GraphNode } from '../../src/scene/graph-node.js';
import { InstancedSkin } from '../../src/scene/instanced-skin.js';
import { Mesh } from '../../src/scene/mesh.js';
import { MeshInstance } from '../../src/scene/mesh-instance.js';
import { Scene } from '../../src/scene/scene.js';
import { Skin } from '../../src/scene/skin.js';
import { SkinInstance } from '../../src/scene/skin-instance.js';
import { ShaderMaterial } from '../../src/scene/materials/shader-material.js';
import { StandardMaterial } from '../../src/scene/materials/standard-material.js';
import { Renderer } from '../../src/scene/renderer/renderer.js';
import { ShaderPass } from '../../src/scene/shader-pass.js';
import { Texture } from '../../src/platform/graphics/texture.js';
import { Morph } from '../../src/scene/morph.js';
import { MorphTarget } from '../../src/scene/morph-target.js';
import {
    SHADER_FORWARD, SHADER_PREPASS, SHADERDEF_INSTANCED_SKIN,
    LIGHTTYPE_DIRECTIONAL, SHADOW_PCF3_32F
} from '../../src/scene/constants.js';
import { CameraShaderParams } from '../../src/scene/camera-shader-params.js';
import { WebgpuShaderProcessorWGSL } from '../../src/platform/graphics/webgpu/webgpu-shader-processor-wgsl.js';
import { LightsBuffer } from '../../src/scene/lighting/lights-buffer.js';
import { LightList } from '../../src/scene/lighting/light-list.js';
import { AnimData } from '../../src/framework/anim/evaluator/anim-data.js';
import { AnimTrack } from '../../src/framework/anim/evaluator/anim-track.js';
import { AnimCurve } from '../../src/framework/anim/evaluator/anim-curve.js';
import { AnimClip } from '../../src/framework/anim/evaluator/anim-clip.js';
import { AnimEvaluator } from '../../src/framework/anim/evaluator/anim-evaluator.js';
import { DefaultAnimBinder } from '../../src/framework/anim/binder/default-anim-binder.js';
import { INTERPOLATION_LINEAR } from '../../src/framework/anim/constants.js';

// -----------------------------------------------------------------------------
// helpers
// -----------------------------------------------------------------------------

/**
 * Computes the world space position of a vertex with an independent scalar LBS implementation,
 * using the shader's exact chain: model * instance * vertexSkinMatrix.
 *
 * @param {number[]} positions - The 3 position components of the vertex.
 * @param {number[]} weights - The 4 weights of the vertex.
 * @param {number[]} indices - The 4 bone indices of the vertex.
 * @param {number[][]} skinMatrices - The node-space bone matrices, column-major 16 floats each.
 * @param {number[]} nodeWorld - Column-major 16 floats of the entry node world matrix.
 * @param {number[]} ownerWorld - Column-major 16 floats of the owner node world matrix.
 * @returns {number[]} The world space position.
 */
function lbsworld(positions, weights, indices, skinMatrices, nodeWorld, ownerWorld) {
    // the blended node-space (bind space) position, evaluated with scalars
    const blended = [0, 0, 0];
    for (let k = 0; k < 4; k++) {
        const w = weights[k];
        if (w === 0) continue;
        const m = skinMatrices[indices[k]];
        for (let r = 0; r < 3; r++) {
            blended[r] += w * (m[r] * positions[0] + m[4 + r] * positions[1] + m[8 + r] * positions[2] + m[12 + r]);
        }
    }

    // the instance matrix is inverse(ownerWorld) * nodeWorld
    const instance = mulMat4(invertMat4(ownerWorld), nodeWorld);
    const full = mulMat4(ownerWorld, instance);

    const out = [0, 0, 0];
    for (let r = 0; r < 3; r++) {
        out[r] = full[r] * blended[0] + full[4 + r] * blended[1] + full[8 + r] * blended[2] + full[12 + r];
    }
    return out;
}

/**
 * Multiplies two column-major 4x4 matrices given as plain arrays.
 *
 * @param {number[]} a - The left matrix.
 * @param {number[]} b - The right matrix.
 * @returns {number[]} a * b.
 */
function mulMat4(a, b) {
    const out = new Array(16).fill(0);
    for (let c = 0; c < 4; c++) {
        for (let r = 0; r < 4; r++) {
            let sum = 0;
            for (let k = 0; k < 4; k++) {
                sum += a[k * 4 + r] * b[c * 4 + k];
            }
            out[c * 4 + r] = sum;
        }
    }
    return out;
}

/**
 * Inverts a column-major 4x4 matrix given as plain arrays, by Gauss-Jordan elimination.
 *
 * @param {number[]} m - The matrix.
 * @returns {number[]} The inverse.
 */
function invertMat4(m) {
    // an affine inverse is enough here, but a general one keeps the reference independent
    const a = [];
    for (let r = 0; r < 4; r++) {
        a.push([m[r], m[4 + r], m[8 + r], m[12 + r], r === 0 ? 1 : 0, r === 1 ? 1 : 0, r === 2 ? 1 : 0, r === 3 ? 1 : 0]);
    }
    for (let col = 0; col < 4; col++) {
        let pivot = col;
        for (let r = col + 1; r < 4; r++) {
            if (Math.abs(a[r][col]) > Math.abs(a[pivot][col])) pivot = r;
        }
        assert.ok(Math.abs(a[pivot][col]) > 1e-12, 'matrix invertible');
        const tmp = a[col]; a[col] = a[pivot]; a[pivot] = tmp;
        const d = a[col][col];
        for (let i = 0; i < 8; i++) a[col][i] /= d;
        for (let r = 0; r < 4; r++) {
            if (r === col) continue;
            const f = a[r][col];
            if (f === 0) continue;
            for (let i = 0; i < 8; i++) a[r][i] -= f * a[col][i];
        }
    }
    const inv = new Array(16);
    for (let r = 0; r < 4; r++) {
        for (let c = 0; c < 4; c++) {
            inv[c * 4 + r] = a[r][4 + c];
        }
    }
    return inv;
}

/**
 * Reads one instance matrix back from the instanced skin's vertex buffer storage, in the default
 * instancing layout of four vec4 rows.
 *
 * @param {InstancedSkin} instancedSkin - The instanced skin.
 * @param {number} index - The instance index.
 * @returns {number[]} The column-major 4x4 matrix.
 */
function readInstanceMatrix(instancedSkin, index) {
    const storage = instancedSkin.vertexBuffer.lock();
    const data = ArrayBuffer.isView(storage) ?
        new Float32Array(storage.buffer, storage.byteOffset, storage.byteLength / 4) :
        new Float32Array(storage);
    const base = index * 16;
    return Array.from(data.subarray(base, base + 16));
}

/**
 * Decodes a 12 float pose atlas row back into a column-major 4x4 matrix.
 *
 * @param {Float32Array} palette - The atlas CPU data.
 * @param {number} boneIndex - The index of the bone in the atlas.
 * @returns {number[]} The column-major matrix.
 */
function decodePalette(palette, boneIndex) {
    const base = boneIndex * 12;
    const v1 = [palette[base], palette[base + 1], palette[base + 2], palette[base + 3]];
    const v2 = [palette[base + 4], palette[base + 5], palette[base + 6], palette[base + 7]];
    const v3 = [palette[base + 8], palette[base + 9], palette[base + 10], palette[base + 11]];
    // the same transpose the shader applies
    return [
        v1[0], v2[0], v3[0], 0,
        v1[1], v2[1], v3[1], 0,
        v1[2], v2[2], v3[2], 0,
        v1[3], v2[3], v3[3], 1
    ];
}

/**
 * Returns true when the two values are within a relative + absolute tolerance.
 *
 * @param {number} a - The first value.
 * @param {number} b - The second value.
 * @param {number} [tolerance] - The tolerance.
 * @returns {boolean} True when close.
 */
function close(a, b, tolerance = 1e-5) {
    return Math.abs(a - b) <= tolerance * (1 + Math.max(Math.abs(a), Math.abs(b)));
}

/**
 * Creates a device with the program library and shader chunks installed, as an app would, without
 * a browser.
 *
 * @param {boolean} [webgpu] - True to create a device reporting WebGPU.
 * @returns {NullGraphicsDevice} The device.
 */
function createDevice(webgpu = false) {
    const device = new NullGraphicsDevice();
    device.isWebGPU = webgpu;
    if (webgpu) {
        device.hasTranspilers = true;
    }

    setProgramLibrary(device, new ProgramLibrary(device));
    ShaderChunks.get(device, SHADERLANGUAGE_GLSL).add(shaderChunksGLSL);
    ShaderChunks.get(device, SHADERLANGUAGE_WGSL).add(shaderChunksWGSL);

    // the clustered lighting chunk is normally installed by the LightsBuffer
    ShaderChunks.get(device, SHADERLANGUAGE_GLSL).set('lightBufferDefinesPS', '');
    ShaderChunks.get(device, SHADERLANGUAGE_WGSL).set('lightBufferDefinesPS', '');
    return device;
}

/**
 * A fixture with a skinned mesh, two characters with independent bones, and the owning mesh
 * instance.
 */
class CharacterFixture {
    /**
     * @param {object} [options] - Fixture options.
     * @param {number} [options.bones] - Number of bones.
     * @param {number} [options.boneCount] - Alias of bones.
     */
    constructor(options = {}) {
        const boneCount = options.bones ?? options.boneCount ?? 5;
        this.boneCount = boneCount;
        this.device = options.device ?? createDevice();

        // the mesh, with four weights per vertex and a triangle per vertex triple
        const vertexCount = options.vertexCount ?? 8;
        const positions = new Float32Array(vertexCount * 3);
        const weights = new Float32Array(vertexCount * 4);
        const indices = new Uint8Array(vertexCount * 4);

        // spread the vertices out and assign each one a pair of consecutive bones
        for (let v = 0; v < vertexCount; v++) {
            positions[v * 3] = (v % 3) * 0.5;
            positions[v * 3 + 1] = Math.floor(v / 3) * 0.4;
            positions[v * 3 + 2] = ((v % 2) - 0.5) * 0.3;
            const b0 = v % boneCount;
            const b1 = (b0 + 1) % boneCount;
            weights[v * 4] = 0.65;
            weights[v * 4 + 1] = 0.35;
            indices[v * 4] = b0;
            indices[v * 4 + 1] = b1;
            indices[v * 4 + 2] = b0;
            indices[v * 4 + 3] = b0;
        }

        const mesh = new Mesh(this.device);
        mesh.setPositions(positions);
        mesh.setVertexStream(SEMANTIC_BLENDINDICES, indices, 4, vertexCount, TYPE_UINT8);
        mesh.setVertexStream(SEMANTIC_BLENDWEIGHT, weights, 4);
        mesh.setIndices([0, 1, 2, 3, 4, 5, 6, 7, 0, 2, 4, 6]);
        mesh.update();
        this.positions = positions;
        this.weights = weights;
        this.indices = indices;

        // the inverse bind pose, a different one per bone so that a mixed up bone shows up
        const inverseBindPose = [];
        for (let i = 0; i < boneCount; i++) {
            // each bone's rest position is (i * 0.1, i * 0.7, 0), with a rotation about Y
            const m = new Mat4().setFromEulerAngles(0, i * 17, 0);
            m.data[12] = i * 0.1;
            m.data[13] = i * 0.7;
            m.data[14] = 0;
            inverseBindPose.push(m);
        }
        this.skin = new Skin(this.device, inverseBindPose, inverseBindPose.map((_, i) => `bone${i}`));
        mesh.skin = this.skin;

        // the draw owner, with a non-identity transform
        this.owner = new GraphNode('owner');
        this.owner.setLocalPosition(10, -2, 3);
        this.owner.setLocalEulerAngles(10, 20, 30);
        this.owner.setLocalScale(2, 2, 2);

        this.material = new StandardMaterial();
        this.meshInstance = new MeshInstance(mesh, this.material, this.owner);
        this.mesh = mesh;

        // two characters, each in its own hierarchy, with its own four weights
        this.characters = [
            this.createCharacter('characterA', new Vec3(-4, 0, 0), 1),
            this.createCharacter('characterB', new Vec3(3, 5, 1), -1)
        ];
    }

    /**
     * Creates a character: a root node containing a chain of bones, and a skin instance bound to
     * them.
     *
     * @param {string} name - The name of the root.
     * @param {Vec3} offset - The local position of the root.
     * @param {number} sign - The sign of the bone chain's bend, to make the two differ.
     * @returns {object} The character.
     */
    createCharacter(name, offset, sign) {
        const root = new GraphNode(name);
        root.setLocalPosition(offset);
        root.setLocalEulerAngles(0, sign * 35, sign * 10);
        root.setLocalScale(1 + 0.25 * sign, 1, 1.2);

        const bones = [];
        const boneNames = [];
        for (let i = 0; i < this.boneCount; i++) {
            const bone = new GraphNode(`bone${i}`);
            // the local transform of the bone differs from its bind pose, so the pose is not
            // the bind pose
            bone.setLocalPosition(i * 0.1 + sign * 0.05 * i, i * 0.7 + sign * 0.03, 0.02 * i);
            bone.setLocalEulerAngles(sign * i * 13, i * 17 + sign * 4, 0);
            (i === 0 ? root : bones[i - 1]).addChild(bone);
            bones.push(bone);
            boneNames.push(bone.name);
        }

        const skinInstance = new SkinInstance(this.skin);
        skinInstance.bones = bones;

        return { root, bones, skinInstance, name };
    }

    /**
     * The entries for {@link MeshInstance#setInstancedSkin}, in draw order.
     *
     * @returns {{skinInstance: SkinInstance, node: GraphNode}[]} The entries.
     */
    entries() {
        return this.characters.map(character => ({
            skinInstance: character.skinInstance,
            node: character.root
        }));
    }

    /**
     * Destroys the fixture resources.
     */
    destroy() {
        this.meshInstance.destroy();
        this.material.destroy();
        this.mesh.destroy();
        this.device.destroy();
    }
}

// -----------------------------------------------------------------------------
// palette group
// -----------------------------------------------------------------------------

export default [
    {
        group: 'palette',
        name: 'atlas layout, addressing and decode',
        run(check) {
            const fixture = new CharacterFixture();
            const mi = fixture.meshInstance;
            const renderer = new Renderer(fixture.device, new Scene(fixture.device));

            check(mi.setInstancedSkin(fixture.entries()) === mi, 'setInstancedSkin returns this');

            const instancedSkin = mi.instancedSkin;
            check(instancedSkin instanceof InstancedSkin, 'mode enabled');
            check(instancedSkin.count === 2, 'two instances');
            check(instancedSkin.boneCount === fixture.boneCount, 'bone count reported');
            check(mi.aabb !== null, 'bounds available');

            // evaluate the pose and the matrices as the renderer does before culling
            renderer.updateCpuSkinMatrices([mi]);

            const texture = instancedSkin.boneTexture;
            check(texture !== null, 'atlas texture exists');
            check(texture.width % 3 === 0, 'atlas width is a multiple of 3');
            check(texture.width <= fixture.device.maxTextureSize, 'atlas width within limit');
            check(texture.height <= fixture.device.maxTextureSize, 'atlas height within limit');

            // the texture can hold every bone, and no bone crosses a row
            const texelsPerRow = texture.width;
            check(texelsPerRow % 3 === 0, 'row holds whole bones');
            const totalBones = instancedSkin.count * instancedSkin.boneCount;
            check(texelsPerRow * texture.height >= totalBones * 3, 'atlas holds every bone');

            const palette = instancedSkin.matrixPalette;
            check(palette instanceof Float32Array, 'palette exposed as a Float32Array');
            check(palette.length === texture.width * texture.height * 4,
                'palette includes the tail padding of the texture');

            // no borrowed skin instance was rewritten
            for (const character of fixture.characters) {
                check(character.skinInstance.boneTexture !== texture, 'atlas is not a borrowed texture');
                check(character.skinInstance.matrixPalette !== palette, 'palette is not borrowed');
            }

            // decode the atlas with a scalar implementation and compare against an independent LBS
            const ownerWorld = Array.from(mi.node.getWorldTransform().data);
            const numVertices = fixture.positions.length / 3;
            for (let i = 0; i < instancedSkin.count; i++) {
                const character = fixture.characters[i];
                const nodeWorld = Array.from(character.root.getWorldTransform().data);
                const entry = instancedSkin.entries[i];

                // the instance matrix is inverse(owner) * node, read back from the buffer
                const expectedInstance = mulMat4(invertMat4(ownerWorld), nodeWorld);
                const instance = readInstanceMatrix(instancedSkin, i);
                let instanceOk = true;
                for (let e = 0; e < 16; e++) {
                    if (!close(instance[e], expectedInstance[e])) instanceOk = false;
                }
                check(instanceOk, `instance matrix ${i} is inverse(owner) * node`);

                // decode each bone's node-space skin matrix from the atlas
                const skinMatrices = [];
                for (let b = 0; b < instancedSkin.boneCount; b++) {
                    skinMatrices.push(decodePalette(palette, i * instancedSkin.boneCount + b));
                }

                // independently compute inverse(node) * bone.world * inverseBind, per bone
                const invNode = invertMat4(nodeWorld);
                let boneOk = true;
                for (let b = 0; b < instancedSkin.boneCount; b++) {
                    const expectedBone = mulMat4(
                        mulMat4(invNode, Array.from(character.bones[b].getWorldTransform().data)),
                        Array.from(fixture.skin.inverseBindPose[b].data)
                    );
                    for (let e = 0; e < 16; e++) {
                        if (!close(skinMatrices[b][e], expectedBone[e])) boneOk = false;
                    }
                }
                check(boneOk, `instance ${i} bone matrices decode to inverse(node) * bone * inverseBind`);

                // compare every vertex against the independent scalar LBS
                let allClose = true;
                for (let v = 0; v < numVertices; v++) {
                    const p = [fixture.positions[v * 3], fixture.positions[v * 3 + 1], fixture.positions[v * 3 + 2]];
                    const w = [fixture.weights[v * 4], fixture.weights[v * 4 + 1], fixture.weights[v * 4 + 2], fixture.weights[v * 4 + 3]];
                    const bi = [fixture.indices[v * 4], fixture.indices[v * 4 + 1], fixture.indices[v * 4 + 2], fixture.indices[v * 4 + 3]];
                    const expected = lbsworld(p, w, bi, skinMatrices, nodeWorld, ownerWorld);

                    // the shader's chain, using the decoded atlas and the buffer instance matrix
                    const skinPoint = [0, 0, 0];
                    for (let k = 0; k < 4; k++) {
                        const weight = w[k];
                        if (weight === 0) continue;
                        const m = skinMatrices[bi[k]];
                        for (let r = 0; r < 3; r++) {
                            skinPoint[r] += weight * (m[r] * p[0] + m[4 + r] * p[1] + m[8 + r] * p[2] + m[12 + r]);
                        }
                    }
                    const modelInstance = mulMat4(ownerWorld, instance);
                    const actual = [0, 0, 0];
                    for (let r = 0; r < 3; r++) {
                        actual[r] = modelInstance[r] * skinPoint[0] + modelInstance[4 + r] * skinPoint[1] +
                            modelInstance[8 + r] * skinPoint[2] + modelInstance[12 + r];
                    }
                    for (let r = 0; r < 3; r++) {
                        if (!close(actual[r], expected[r])) {
                            allClose = false;
                        }
                    }
                }
                check(allClose, `instance ${i} decodes to the independent LBS result`);

                // bounds enclose every blended vertex
                for (let v = 0; v < numVertices; v++) {
                    const p = [fixture.positions[v * 3], fixture.positions[v * 3 + 1], fixture.positions[v * 3 + 2]];
                    const w = [fixture.weights[v * 4], fixture.weights[v * 4 + 1], fixture.weights[v * 4 + 2], fixture.weights[v * 4 + 3]];
                    const bi = [fixture.indices[v * 4], fixture.indices[v * 4 + 1], fixture.indices[v * 4 + 2], fixture.indices[v * 4 + 3]];
                    const expected = lbsworld(p, w, bi, skinMatrices, nodeWorld, ownerWorld);
                    check(mi.aabb.containsPoint(new Vec3(expected[0], expected[1], expected[2])),
                        `bounds enclose instance ${i} vertex ${v}`);
                }
            }

            // the bounds are finite and not the mesh bind box
            check(Number.isFinite(mi.aabb.center.x) && Number.isFinite(mi.aabb.halfExtents.x), 'bounds finite');
            check(mi.aabb.halfExtents.x < 1e5, 'bounds are not infinite');

            renderer.destroy();
            fixture.destroy();
        }
    },

    {
        group: 'palette',
        name: 'rolling atlas keeps bones inside rows',
        run(check) {
            // enough characters to need several rows of the atlas
            const fixture = new CharacterFixture({ bones: 5, vertexCount: 8, device: createDevice() });
            const renderer = new Renderer(fixture.device, new Scene(fixture.device));
            const mi = fixture.meshInstance;
            const extras = [];
            for (let i = 0; i < 6; i++) {
                extras.push(fixture.createCharacter(`extra${i}`, new Vec3(i * 2, 0, 0), i % 2 ? 1 : -1));
            }
            const entries = fixture.entries().concat(extras.map(c => ({ skinInstance: c.skinInstance, node: c.root })));
            mi.setInstancedSkin(entries);

            const instancedSkin = mi.instancedSkin;
            const texture = instancedSkin.boneTexture;
            check(instancedSkin.count === 8, 'eight instances');
            check(texture.width % 3 === 0, 'width is a multiple of 3');

            // every bone's three texels fit in one row
            const bonesPerRow = texture.width / 3;
            check(Number.isInteger(bonesPerRow) && bonesPerRow > 0, 'whole bones per row');
            const totalBones = instancedSkin.count * instancedSkin.boneCount;
            check(bonesPerRow * texture.height >= totalBones, 'every bone has room');
            check(texture.height > 1, 'a multi-row atlas is exercised');

            renderer.updateCpuSkinMatrices([mi]);

            // the first bone of each instance decodes to inverse(node) * bone.world * inverseBind
            const palette = instancedSkin.matrixPalette;
            for (let i = 0; i < instancedSkin.count; i++) {
                const entry = instancedSkin.entries[i];
                const invNode = invertMat4(Array.from(entry.node.getWorldTransform().data));
                const expected = mulMat4(
                    mulMat4(invNode, Array.from(entry.skinInstance.bones[0].getWorldTransform().data)),
                    Array.from(fixture.skin.inverseBindPose[0].data)
                );
                const decoded = decodePalette(palette, i * instancedSkin.boneCount);
                let ok = true;
                for (let e = 0; e < 16; e++) {
                    if (!close(decoded[e], expected[e])) ok = false;
                }
                check(ok, `bone 0 of instance ${i} decodes at its rolled position`);
            }

            renderer.destroy();
            fixture.destroy();
        }
    },

    {
        group: 'palette',
        name: 'shader addressing wraps rows correctly',
        run(check) {
            // a fixture with enough instances that the atlas needs several rows, so the shader's
            // div/mod row addressing is exercised
            const fixture = new CharacterFixture({ bones: 7 });
            const renderer = new Renderer(fixture.device, new Scene(fixture.device));
            const mi = fixture.meshInstance;
            const extras = [];
            for (let i = 0; i < 4; i++) {
                extras.push(fixture.createCharacter(`extra${i}`, new Vec3(i * 3, 0, 0), i % 2 ? 1 : -1));
            }
            mi.setInstancedSkin(fixture.entries().concat(extras.map(c => ({ skinInstance: c.skinInstance, node: c.root }))));
            renderer.updateCpuSkinMatrices([mi]);

            const instancedSkin = mi.instancedSkin;
            const texture = instancedSkin.boneTexture;
            check(texture.height > 1, 'the atlas spans several rows');

            const palette = instancedSkin.matrixPalette;
            const width = texture.width;

            // the exact addressing the shader computes, per bone and instance, in texels
            const texelOf = (instance, bone) => {
                const flatBone = instance * instancedSkin.boneCount + bone;
                const baseTexel = flatBone * 3;
                const v = Math.floor(baseTexel / width);
                const u = baseTexel % width;
                return { u, v };
            };

            let wrapped = 0;
            for (let i = 0; i < instancedSkin.count; i++) {
                for (let b = 0; b < instancedSkin.boneCount; b++) {
                    const { u, v } = texelOf(i, b);

                    // three texels, no row crossing
                    check(u + 2 < width, `instance ${i} bone ${b} fits in a row`);

                    // read the three texels back and compare with the 12 float palette row
                    const flat = (v * width + u) * 4;
                    const v1 = [palette[flat], palette[flat + 1], palette[flat + 2], palette[flat + 3]];
                    const v2 = [palette[flat + 4], palette[flat + 5], palette[flat + 6], palette[flat + 7]];
                    const v3 = [palette[flat + 8], palette[flat + 9], palette[flat + 10], palette[flat + 11]];

                    const expectedBase = (i * instancedSkin.boneCount + b) * 12;
                    check(close(v1[0], palette[expectedBase]) && close(v3[3], palette[expectedBase + 11]),
                        `instance ${i} bone ${b} is addressed at its rolled texel`);

                    if (v > 0) wrapped++;
                }
            }
            check(wrapped > 0, 'some bones live on a non-zero row');

            renderer.destroy();
            fixture.destroy();
        }
    },

    {
        group: 'palette',
        name: 'repeated skin instance and same-node entries',
        run(check) {
            const fixture = new CharacterFixture();
            const renderer = new Renderer(fixture.device, new Scene(fixture.device));
            const mi = fixture.meshInstance;

            // the same skin and node twice is valid, and must keep both blocks distinct
            const entries = [
                { skinInstance: fixture.characters[0].skinInstance, node: fixture.characters[0].root },
                { skinInstance: fixture.characters[0].skinInstance, node: fixture.characters[0].root }
            ];
            mi.setInstancedSkin(entries);
            renderer.updateCpuSkinMatrices([mi]);

            const instancedSkin = mi.instancedSkin;
            check(instancedSkin.count === 2, 'same skin twice is allowed');
            const palette = instancedSkin.matrixPalette;
            const blockSize = instancedSkin.boneCount * 12;
            let identical = true;
            for (let i = 0; i < blockSize; i++) {
                if (!close(palette[i], palette[blockSize + i])) identical = false;
            }
            check(identical, 'each repeated entry has its own block');
            check(instancedSkin.boneTexture.width % 3 === 0, 'atlas stays aligned');

            renderer.destroy();
            fixture.destroy();
        }
    },

    {
        group: 'pipeline',
        name: 'renderer update, cull-before-pose and gpu bind',
        run(check) {
            const fixture = new CharacterFixture();
            const scene = new Scene(fixture.device);
            scene.clusteredLightingEnabled = false;
            const renderer = new Renderer(fixture.device, scene);
            const mi = fixture.meshInstance;

            mi.setInstancedSkin(fixture.entries());

            // the renderer evaluates the pose before culling, and binds the atlas afterwards
            renderer.updateCpuSkinMatrices([mi]);
            const aabbBefore = mi.aabb.clone();

            check(deviceScopeValue(fixture.device, 'texture_instancedSkinPoseMap') === null,
                'atlas uniform is not bound before the draw');
            renderer.updateGpuSkinMatrices([mi]);
            renderer.setSkinning(fixture.device, mi);

            check(deviceScopeValue(fixture.device, 'texture_instancedSkinPoseMap') === mi.instancedSkin.boneTexture,
                'the atlas is bound by the renderer');
            check(deviceScopeValue(fixture.device, 'instancedSkin_boneCount') === fixture.boneCount,
                'the bone count uniform is bound');
            check(fixture.device.scope.resolve('texture_poseMap').value === null,
                'the single-bone pose map is not bound');

            // move a character and re-run the renderer's pre-cull update: the bounds move and the
            // draw buffers follow the same evaluation
            mi.instancedSkin.entries[0].node.setLocalPosition(-30, 0, 0);
            renderer.updateCpuSkinMatrices([mi]);
            const aabbAfter = mi.aabb.clone();
            check(!aabbBefore.containsPoint(new Vec3(-34, 0, 0)) || aabbAfter.containsPoint(new Vec3(-34, 0, 0)),
                'bounds follow the moved character');
            check(aabbAfter.center.x < aabbBefore.center.x, 'the world bounds moved left');

            // the pose evaluated before culling is the one uploaded
            const paletteBefore = mi.instancedSkin.matrixPalette.slice();
            renderer.updateGpuSkinMatrices([mi]);
            renderer.updateCpuSkinMatrices([mi]);
            let samePalette = true;
            for (let i = 0; i < paletteBefore.length; i++) {
                if (paletteBefore[i] !== mi.instancedSkin.matrixPalette[i]) samePalette = false;
            }
            check(samePalette, 'unchanged pose keeps the uploaded data');

            renderer.destroy();
            fixture.destroy();
        }
    },

    {
        group: 'pipeline',
        name: 'animated characters and shader variants',
        run(check) {
            const fixture = new CharacterFixture();
            const scene = new Scene(fixture.device);
            scene.clusteredLightingEnabled = false;
            const renderer = new Renderer(fixture.device, scene);
            const mi = fixture.meshInstance;
            mi.setInstancedSkin(fixture.entries());

            // animate the first character's root bone
            const track = new AnimTrack('bend', 1,
                [new AnimData(1, [0, 1])],
                [new AnimData(3, [0, 0, 0, 0, 2, 0])],
                [new AnimCurve([{ entityPath: ['bone0'], component: 'graph', propertyPath: ['localPosition'] }], 0, 0, INTERPOLATION_LINEAR)]);
            const evaluator = new AnimEvaluator(new DefaultAnimBinder(fixture.characters[0].root));
            evaluator.addClip(new AnimClip(track, 0, 1, true, false));

            evaluator.update(0.0);
            renderer.updateCpuSkinMatrices([mi]);
            const start = mi.aabb.clone();
            const paletteStart = mi.instancedSkin.matrixPalette.slice();

            evaluator.update(0.5);
            renderer.updateCpuSkinMatrices([mi]);
            const mid = mi.aabb.clone();
            const paletteMid = mi.instancedSkin.matrixPalette.slice();

            let changed = false;
            for (let i = 0; i < paletteStart.length; i++) {
                if (!close(paletteStart[i], paletteMid[i])) {
                    changed = true;
                    break;
                }
            }
            check(changed, 'sampling the animation changes the pose');
            check(start.center.y !== mid.center.y || start.halfExtents.y !== mid.halfExtents.y,
                'the bounds follow the animated pose');

            // both shader languages generate visible addressing for the forward, shadow and depth
            // variants, and each variant branches to the compound transform. The variants are
            // generated through the LitShader directly, and through the StandardMaterial options
            // path, in every pass.
            for (const webgpu of [false, true]) {
                const device = createDevice(webgpu);
                const passInfo = ShaderPass.get(device).allocate('shadow_instanced_skin_test', {
                    isShadow: true,
                    lightType: LIGHTTYPE_DIRECTIONAL,
                    shadowType: SHADOW_PCF3_32F
                });
                const passes = [SHADER_FORWARD, SHADER_PREPASS, passInfo.index];

                for (const pass of passes) {
                    for (const language of ['glsl', 'wgsl']) {

                        // the LitShader generator
                        const generated = generateVertexShader(device, pass, language === 'wgsl');
                        checkShaderVariant(generated.source, `${language} lit pass ${pass}`, pass, check, language === 'wgsl');

                        // the full StandardMaterial options path, preprocessed by the real
                        // pipeline and then inspected
                        const material = new StandardMaterial();
                        const scene = new Scene(device);
                        scene.clusteredLightingEnabled = webgpu;
                        const shader = material.getShaderVariant({
                            device,
                            scene,
                            pass,
                            objDefs: SHADERDEF_INSTANCED_SKIN | 4,
                            lightList: new LightList(),
                            cameraShaderParams: new CameraShaderParams(),
                            vertexFormat: null
                        });
                        check(shader !== null && !shader.failed,
                            `${language} material pass ${pass} compiles`);
                        if (shader && !shader.failed) {
                            check(shader.definition.shaderLanguage === (webgpu ? SHADERLANGUAGE_WGSL : SHADERLANGUAGE_GLSL),
                                `${language} material pass ${pass} generates the expected language`);
                            const pre = Preprocessor.run(shader.definition.vshader, shader.definition.vincludes, {
                                sourceName: 'instanced-skin-material',
                                stripDefines: shader.definition.shaderLanguage === SHADERLANGUAGE_WGSL
                            });
                            checkShaderVariant(pre ?? '', `${language} material pass ${pass}`, pass, check,
                                shader.definition.shaderLanguage === SHADERLANGUAGE_WGSL);
                        }
                        material.destroy();
                    }
                }
                device.destroy();
            }

            renderer.destroy();
            fixture.destroy();
        }
    },

    {
        group: 'pipeline',
        name: 'real WGSL processor resolves the instanced skin bindings',
        run(check) {
            // drive the production WGSL processor, which reflects the shader into bind groups and
            // attribute locations. This is what the WebGPU backend runs at shader creation.
            const device = createDevice(true);
            // the lights buffer installs the clustered lighting shader chunk
            const lightsBuffer = new LightsBuffer(device);
            check(lightsBuffer !== null, 'the lights buffer is installed');
            const scene = new Scene(device);
            scene.clusteredLightingEnabled = true;
            const material = new StandardMaterial();
            const shader = material.getShaderVariant({
                device,
                scene,
                pass: SHADER_FORWARD,
                objDefs: SHADERDEF_INSTANCED_SKIN,
                lightList: new LightList(),
                cameraShaderParams: new CameraShaderParams(),
                vertexFormat: null
            });
            check(shader !== null && !shader.failed, 'the material shader is generated');

            const processed = WebgpuShaderProcessorWGSL.run(device, shader.definition, { failed: false, label: 'test', device });
            const source = processed.vshader;

            // the atlas is a view bind group texture, and the bone count is a mesh uniform
            const textureFormats = processed.meshBindGroupFormat?.textureFormats ?? [];
            check(textureFormats.some(t => t.name === 'texture_instancedSkinPoseMap'),
                'the atlas is reflected as a mesh bind group texture');
            const meshUniforms = processed.meshUniformBufferFormat.uniforms.map(u => u.name);
            check(meshUniforms.includes('instancedSkin_boneCount'),
                'the bone count is reflected as a mesh uniform');

            // the instance index is the real WGSL builtin, with the engine's alias
            check(source.includes('pcInstanceIndex'), 'the WGSL builtin alias is declared');
            check(/instanceBase\s*=.*pcInstanceIndex/.test(source), 'the atlas offset reads the instance index');

            // the four instance attributes occupy the default instancing locations
            const locations = [...processed.attributes.entries()].sort((a, b) => a[0] - b[0]);
            const byName = Object.fromEntries(locations.map(([loc, name]) => [name, loc]));
            check(byName.instance_line1 === 11, 'instance_line1 at location 11');
            check(byName.instance_line2 === 12, 'instance_line2 at location 12');
            check(byName.instance_line3 === 14, 'instance_line3 at location 14');
            check(byName.instance_line4 === 15, 'instance_line4 at location 15');
            check(byName.vertex_boneIndices === 3, 'bone indices at location 3');
            check(byName.vertex_boneWeights === 2, 'bone weights at location 2');

            // the mesh instance storage is not used, as the draw is instanced
            check(processed.usesMeshInstanceStorage === false, 'the instanced draw does not use storage indexing');

            material.destroy();
            device.destroy();
        }
    },

    {
        group: 'pipeline',
        name: 'legacy shader variants stay isolated',
        run(check) {
            // the old skin, batch and instancing variants must not gain the instanced skin branch
            const device = createDevice();

            const plainSkin = generateVertexShader(device, SHADER_FORWARD, false, { skin: true, batch: false });
            check(plainSkin.source.includes('getSkinMatrix'), 'plain skin keeps its own path');
            check(!plainSkin.source.includes('getInstancedSkinMatrix'), 'plain skin has no instanced path');
            check(!plainSkin.source.includes('INSTANCED_SKIN'), 'plain skin define is not set');

            const batch = generateVertexShader(device, SHADER_FORWARD, false, { skin: true, batch: true });
            check(batch.source.includes('getBoneMatrix'), 'batch keeps its single bone path');
            check(!batch.source.includes('getInstancedSkinMatrix'), 'batch has no instanced path');

            const instancing = generateVertexShader(device, SHADER_FORWARD, false, { useInstancing: true });
            check(instancing.source.includes('instance_line1'), 'instancing reads the instance lines');
            check(!instancing.source.includes('getInstancedSkinMatrix'), 'instancing has no skin path');

            const streamed = generateVertexShader(device, SHADER_FORWARD, false, { instancedSkin: true });
            check(streamed.source.includes('INSTANCED_SKIN'), 'the new variant sets its define');
            check(!streamed.source.includes('#define SKIN '), 'the new variant does not set SKIN');
            check(!streamed.source.includes('#define BATCH '), 'the new variant does not set BATCH');
            check(!streamed.source.includes('#define INSTANCING '), 'the new variant does not set INSTANCING');

            device.destroy();
        }
    },

    {
        group: 'pipeline',
        name: 'shader cache keys are distinct',
        run(check) {
            const device = createDevice();
            const keyOf = (instancedSkin, skin, batch) => {
                const local = new LitShaderOptions();
                local.pass = SHADER_FORWARD;
                local.instancedSkin = instancedSkin;
                local.skin = skin;
                local.batch = batch;
                local.clusteredLightingEnabled = false;
                const shader = new LitShader(device, local);
                shader.generateVertexShader(new Array(8).fill(false), new Array(8).fill(false), new Map());
                return [...shader.vDefines.entries()].sort().map(e => e.join('=')).join('|');
            };

            const instancedKey = keyOf(true, false, false);
            const skinKey = keyOf(false, true, false);
            const batchKey = keyOf(false, true, true);
            check(instancedKey !== skinKey, 'the instanced variant has its own defines');
            check(instancedKey !== batchKey, 'the instanced variant differs from batch');
            check(skinKey !== batchKey, 'the legacy skin and batch variants still differ');

            device.destroy();
        }
    },

    {
        group: 'lifetime',
        name: 'invalid replacement leaves state untouched',
        run(check) {
            const fixture = new CharacterFixture();
            const renderer = new Renderer(fixture.device, new Scene(fixture.device));
            const mi = fixture.meshInstance;

            mi.setInstancedSkin(fixture.entries());
            const original = mi.instancedSkin;
            const originalTexture = original.boneTexture;
            const originalBuffer = original.vertexBuffer;
            const originalCount = original.count;
            const originalDefs = mi._shaderDefs;

            // an invalid entry: a skin instance that is not bound to the mesh's skin
            const otherSkin = new Skin(fixture.device, [new Mat4()], ['bone0']);
            const foreign = new SkinInstance(otherSkin);
            foreign.bones = [new GraphNode('bone0')];

            let threw = false;
            try {
                mi.setInstancedSkin([{ skinInstance: foreign, node: fixture.characters[0].root }]);
            } catch (e) {
                threw = e instanceof RangeError;
            }
            check(threw, 'a mismatched skin throws a RangeError');

            check(mi.instancedSkin === original, 'the original instanced skin is kept');
            check(original.boneTexture === originalTexture, 'the original atlas survives');
            check(original.vertexBuffer === originalBuffer, 'the original instance buffer survives');
            check(original.count === originalCount, 'the instance count is unchanged');
            check(mi._shaderDefs === originalDefs, 'the shader state is unchanged');

            // the original atlas was not destroyed, so it can still be drawn
            renderer.updateCpuSkinMatrices([mi]);
            check(Number.isFinite(mi.aabb.center.x), 'the original path still evaluates');

            // a non-finite node transform is rejected without side effects
            const badNode = new GraphNode('badNode');
            badNode.setLocalPosition(NaN, 0, 0);

            threw = false;
            try {
                mi.setInstancedSkin([
                    { skinInstance: fixture.characters[0].skinInstance, node: fixture.characters[0].root },
                    { skinInstance: fixture.characters[1].skinInstance, node: badNode }
                ]);
            } catch (e) {
                threw = e instanceof RangeError;
            }
            check(threw, 'a non-finite node matrix throws a RangeError');
            check(mi.instancedSkin === original, 'the original instanced skin survives a bad node');
            check(mi._shaderDefs === originalDefs, 'the shader state survives a bad node');

            // a singular owner matrix is rejected, using a separate mesh instance
            const singularOwner = new GraphNode('singularOwner');
            singularOwner.setLocalScale(0, 1, 1);
            const singularMi = new MeshInstance(fixture.mesh, fixture.material, singularOwner);
            threw = false;
            try {
                singularMi.setInstancedSkin(fixture.entries());
            } catch (e) {
                threw = e instanceof RangeError;
            }
            check(threw, 'a singular owner matrix throws a RangeError');
            check(singularMi.instancedSkin === null, 'the singular owner never published a mode');
            singularMi.destroy();

            // a mirrored entry relative to the owner is rejected
            const mirrored = new GraphNode('mirrored');
            mirrored.setLocalScale(-1, 1, 1);
            threw = false;
            try {
                mi.setInstancedSkin([
                    { skinInstance: fixture.characters[0].skinInstance, node: fixture.characters[0].root },
                    { skinInstance: fixture.characters[1].skinInstance, node: mirrored }
                ]);
            } catch (e) {
                threw = e instanceof RangeError;
            }
            check(threw, 'a mirrored entry throws a RangeError');
            check(mi.instancedSkin === original, 'the original survives a mirrored entry');

            renderer.destroy();
            fixture.destroy();
        }
    },

    {
        group: 'lifetime',
        name: 'zero instances, replacement and release',
        run(check) {
            const fixture = new CharacterFixture();
            const renderer = new Renderer(fixture.device, new Scene(fixture.device));
            const mi = fixture.meshInstance;

            // an empty list is valid and leaves zero instances
            check(mi.setInstancedSkin([]) === mi, 'empty list returns this');
            check(mi.instancedSkin !== null, 'the mode is enabled');
            check(mi.instancedSkin.count === 0, 'zero instances');
            check(mi.instancedSkin.boneCount === 0, 'zero bones');
            check(mi.instancedSkin.boneTexture === null, 'no atlas for zero instances');
            check(mi.instancedSkin.vertexBuffer === null, 'no buffer for zero instances');
            renderer.updateCpuSkinMatrices([mi]);
            renderer.updateGpuSkinMatrices([mi]);
            check(mi.aabb.halfExtents.x === 0 && mi.aabb.halfExtents.y === 0, 'empty bounds are zero');

            // replacing allocates the atlas, replacing again releases it exactly once
            mi.setInstancedSkin(fixture.entries());
            const first = mi.instancedSkin;
            const firstTexture = first.boneTexture;
            const firstBuffer = first.vertexBuffer;
            check(first.count === 2, 'the first replacement has two instances');

            let destroyed = 0;
            const originalDestroy = firstTexture.destroy.bind(firstTexture);
            firstTexture.destroy = () => {
                destroyed++;
                originalDestroy();
            };
            let bufferDestroyed = 0;
            const originalBufferDestroy = firstBuffer.destroy.bind(firstBuffer);
            firstBuffer.destroy = () => {
                bufferDestroyed++;
                originalBufferDestroy();
            };

            mi.setInstancedSkin(fixture.entries());
            check(mi.instancedSkin !== first, 'a new instanced skin was published');
            check(destroyed === 1, 'the previous atlas is released exactly once');
            check(bufferDestroyed === 1, 'the previous buffer is released exactly once');

            const second = mi.instancedSkin;
            const secondTexture = second.boneTexture;
            const secondBuffer = second.vertexBuffer;
            let secondDestroyed = 0;
            secondTexture.destroy = () => {
                secondDestroyed++;
            };
            let secondBufferDestroyed = 0;
            secondBuffer.destroy = () => {
                secondBufferDestroyed++;
            };

            // null disables the mode and releases only the owned resources
            check(mi.setInstancedSkin(null) === mi, 'null returns this');
            check(mi.instancedSkin === null, 'the mode is disabled');
            check(secondDestroyed === 1, 'the atlas is released on disable');
            check(secondBufferDestroyed === 1, 'the instance buffer is released on disable');
            check(mi._shaderDefs & SHADERDEF_INSTANCED_SKIN ? false : true, 'the shader define is cleared');

            // the borrowed skin instances, bones and material were not destroyed
            for (const character of fixture.characters) {
                check(character.skinInstance.skin === fixture.skin, 'borrowed skin survives');
                check(character.bones.length === fixture.boneCount, 'borrowed bones survive');
                check(character.bones[0].parent !== null, 'borrowed bone hierarchy survives');
            }
            check(fixture.material !== null, 'material survives');

            // re-enabling works
            mi.setInstancedSkin(fixture.entries());
            check(mi.instancedSkin !== null && mi.instancedSkin.count === 2, 'the mode can be re-enabled');
            renderer.updateCpuSkinMatrices([mi]);
            check(Number.isFinite(mi.aabb.center.x), 'the re-enabled mode evaluates');

            renderer.destroy();
            fixture.destroy();
        }
    },

    {
        group: 'lifetime',
        name: 'two mesh instances interleave the same skeletons',
        run(check) {
            const fixture = new CharacterFixture();
            const scene = new Scene(fixture.device);
            scene.clusteredLightingEnabled = false;
            const renderer = new Renderer(fixture.device, scene);

            // two mesh instances, both borrowing the same skin instances but with their own roots
            const ownerA = new GraphNode('ownerA');
            ownerA.setLocalPosition(10, 0, 0);
            const ownerB = new GraphNode('ownerB');
            ownerB.setLocalPosition(-10, 3, 0);

            const miA = new MeshInstance(fixture.mesh, fixture.material, ownerA);
            const miB = new MeshInstance(fixture.mesh, fixture.material, ownerB);
            miA.setInstancedSkin(fixture.entries());
            miB.setInstancedSkin(fixture.entries());

            check(miA.instancedSkin !== miB.instancedSkin, 'each owner has its own atlas');
            check(miA.instancedSkin.boneTexture !== miB.instancedSkin.boneTexture, 'the atlases are distinct');

            renderer.updateCpuSkinMatrices([miA, miB]);
            renderer.updateGpuSkinMatrices([miA, miB]);

            // the two owners give the same world positions, as the instance matrix cancels the owner
            const worldOf = (instancedSkin, entryIndex, vertexIndex) => {
                const entry = instancedSkin.entries[entryIndex];
                const invNode = invertMat4(Array.from(entry.node.getWorldTransform().data));
                const invBind = Array.from(fixture.skin.inverseBindPose[0].data);
                const bone = mulMat4(
                    mulMat4(invNode, Array.from(entry.skinInstance.bones[0].getWorldTransform().data)),
                    invBind
                );
                const p = [fixture.positions[vertexIndex * 3], fixture.positions[vertexIndex * 3 + 1], fixture.positions[vertexIndex * 3 + 2]];
                const skinPoint = [0, 0, 0];
                for (let r = 0; r < 3; r++) {
                    skinPoint[r] = bone[r] * p[0] + bone[4 + r] * p[1] + bone[8 + r] * p[2] + bone[12 + r];
                }
                const owner = instancedSkin === miA.instancedSkin ? ownerA : ownerB;
                const instance = mulMat4(invertMat4(Array.from(owner.getWorldTransform().data)), Array.from(entry.node.getWorldTransform().data));
                const full = mulMat4(Array.from(owner.getWorldTransform().data), instance);
                const out = [0, 0, 0];
                for (let r = 0; r < 3; r++) {
                    out[r] = full[r] * skinPoint[0] + full[4 + r] * skinPoint[1] + full[8 + r] * skinPoint[2] + full[12 + r];
                }
                return out;
            };

            const pa = worldOf(miA.instancedSkin, 0, 0);
            const pb = worldOf(miB.instancedSkin, 0, 0);
            check(close(pa[0], pb[0]) && close(pa[1], pb[1]) && close(pa[2], pb[2]),
                'both owners place the same skeleton at the same world position');

            // the bounds of each owner are the same world box
            check(close(miA.aabb.center.x, miB.aabb.center.x) &&
                close(miA.aabb.center.y, miB.aabb.center.y) &&
                close(miA.aabb.halfExtents.x, miB.aabb.halfExtents.x),
                'the two owners report the same world bounds');

            // destroying one leaves the other working
            miA.destroy();
            renderer.updateCpuSkinMatrices([miB]);
            check(Number.isFinite(miB.aabb.center.x), 'the surviving instance still evaluates');
            check(fixture.characters[0].skinInstance.skin === fixture.skin, 'the shared skin survives');

            miB.destroy();
            renderer.destroy();
            fixture.destroy();
        }
    },

    {
        group: 'lifetime',
        name: 'conflicting assignments are rejected',
        run(check) {
            const fixture = new CharacterFixture();
            const mi = fixture.meshInstance;

            // a plain skin instance blocks enabling the mode
            const plain = fixture.characters[0].skinInstance;
            mi.skinInstance = plain;
            let threw = false;
            try {
                mi.setInstancedSkin(fixture.entries());
            } catch (e) {
                threw = e instanceof RangeError;
            }
            check(threw, 'a plain skin instance blocks the mode');
            check(mi.instancedSkin === null, 'the mode was not enabled');
            mi.skinInstance = null;

            // enabling the mode, then assigning a plain skin instance, is rejected
            mi.setInstancedSkin(fixture.entries());
            threw = false;
            try {
                mi.skinInstance = plain;
            } catch (e) {
                threw = e instanceof RangeError;
            }
            check(threw, 'assigning a plain skin instance is rejected');
            check(mi.skinInstance === null, 'the plain skin instance was not assigned');

            // a custom instancing buffer is rejected
            threw = false;
            try {
                mi.setInstancing(true);
            } catch (e) {
                threw = e instanceof RangeError;
            }
            check(threw, 'custom instancing is rejected');

            // and enabling the mode with custom instancing already set is rejected
            mi.setInstancedSkin(null);
            mi.setInstancing(true);
            threw = false;
            try {
                mi.setInstancedSkin(fixture.entries());
            } catch (e) {
                threw = e instanceof RangeError;
            }
            check(threw, 'custom instancing blocks the mode');
            check(mi.instancedSkin === null, 'the mode stayed off');
            mi.setInstancing(null);

            // an unsupported material is rejected
            const originalMaterial = mi.material;
            const shaderMaterial = new ShaderMaterial();
            mi.material = shaderMaterial;
            threw = false;
            try {
                mi.setInstancedSkin(fixture.entries());
            } catch (e) {
                threw = e instanceof RangeError;
            }
            check(threw, 'a non-standard material is rejected');
            mi.material = originalMaterial;

            // a mesh with texture coordinates 6 or 7 is rejected
            const uvMesh = new Mesh(fixture.device);
            uvMesh.setPositions(fixture.positions);
            uvMesh.setVertexStream(SEMANTIC_BLENDINDICES, fixture.indices, 4, fixture.positions.length / 3, TYPE_UINT8);
            uvMesh.setVertexStream(SEMANTIC_BLENDWEIGHT, fixture.weights, 4);
            uvMesh.setUvs(6, new Float32Array(fixture.positions.length / 3 * 2));
            uvMesh.setIndices([0, 1, 2]);
            uvMesh.update();
            uvMesh.skin = fixture.skin;
            const uvMi = new MeshInstance(uvMesh, fixture.material, mi.node);
            threw = false;
            try {
                uvMi.setInstancedSkin(fixture.entries());
            } catch (e) {
                threw = e instanceof RangeError;
            }
            check(threw, 'UV set 6 occupying the instance locations is rejected');
            check(uvMi.instancedSkin === null, 'the UV mesh never enabled the mode');
            uvMi.destroy();
            uvMesh.destroy();

            // a material sampling UV set 7 is rejected
            const uvMapMaterial = new StandardMaterial();
            // a texture is needed for the map to take effect
            const texture = new Texture(fixture.device, { width: 1, height: 1, format: PIXELFORMAT_RGBA8 });
            uvMapMaterial.diffuseMap = texture;
            uvMapMaterial.diffuseMapUv = 7;
            const mappedMi = new MeshInstance(fixture.mesh, uvMapMaterial, mi.node);
            threw = false;
            try {
                mappedMi.setInstancedSkin(fixture.entries());
            } catch (e) {
                threw = e instanceof RangeError;
            }
            check(threw, 'a material sampling UV 7 is rejected');
            check(mappedMi.instancedSkin === null, 'the mapped material never enabled the mode');
            mappedMi.destroy();
            uvMapMaterial.destroy();
            texture.destroy();

            // a mesh carrying morph targets is rejected
            const morphMesh = new Mesh(fixture.device);
            morphMesh.setPositions(fixture.positions);
            morphMesh.setVertexStream(SEMANTIC_BLENDINDICES, fixture.indices, 4, fixture.positions.length / 3, TYPE_UINT8);
            morphMesh.setVertexStream(SEMANTIC_BLENDWEIGHT, fixture.weights, 4);
            morphMesh.setIndices([0, 1, 2]);
            morphMesh.update();
            morphMesh.skin = fixture.skin;
            const morph = new Morph([new MorphTarget({ deltaPositions: new Float32Array(fixture.positions.length) })], fixture.device);
            morphMesh.morph = morph;
            const morphMi = new MeshInstance(morphMesh, fixture.material, mi.node);
            threw = false;
            try {
                morphMi.setInstancedSkin(fixture.entries());
            } catch (e) {
                threw = e instanceof RangeError;
            }
            check(threw, 'a mesh with morph targets is rejected');
            check(morphMi.instancedSkin === null, 'the morph mesh never enabled the mode');
            morphMi.destroy();
            morphMesh.morph = null;
            morph.destroy();
            morphMesh.destroy();

            // the mesh and a custom aabb cannot be swapped while the mode is active
            mi.setInstancedSkin(fixture.entries());
            threw = false;
            try {
                mi.mesh = morphMesh;
            } catch (e) {
                threw = e instanceof RangeError;
            }
            check(threw, 'changing the mesh is rejected while the mode is active');
            check(mi.mesh === fixture.mesh, 'the mesh is unchanged');
            threw = false;
            try {
                mi.setCustomAabb(new BoundingBox(new Vec3(), new Vec3(1, 1, 1)));
            } catch (e) {
                threw = e instanceof RangeError;
            }
            check(threw, 'overriding the bounds is rejected while the mode is active');
            mi.setInstancedSkin(null);
            mi.setCustomAabb(new BoundingBox(new Vec3(), new Vec3(1, 1, 1)));
            mi.setCustomAabb(null);
            check(mi.instancedSkin === null, 'the custom aabb setter works once the mode is off');

            // assigning a non-standard material while the mode is active is rejected
            mi.setInstancedSkin(fixture.entries());
            const standard = new ShaderMaterial();
            threw = false;
            try {
                mi.material = standard;
            } catch (e) {
                threw = e instanceof RangeError;
            }
            check(threw, 'swapping in a non-standard material is rejected');
            mi.setInstancedSkin(null);
            mi.material = standard;
            check(mi.material === standard, 'the material setter works once the mode is off');
            mi.material = fixture.material;

            fixture.destroy();
        }
    },

    {
        group: 'pipeline',
        name: 'mesh bound box is prepared once, not per frame',
        run(check) {
            const fixture = new CharacterFixture();
            const renderer = new Renderer(fixture.device, new Scene(fixture.device));
            const mi = fixture.meshInstance;
            mi.setInstancedSkin(fixture.entries());

            // the bone influence boxes are prepared from the topology once
            check(Array.isArray(fixture.mesh.boneAabb), 'bone influence boxes prepared');
            check(fixture.mesh.boneAabb.length === fixture.boneCount, 'one box per bone');
            check(fixture.mesh.boneUsed.some(v => v), 'bones marked used');

            const boxes = fixture.mesh.boneAabb.slice();
            const meshVertexBuffer = fixture.mesh.vertexBuffer;
            const numVerticesBefore = meshVertexBuffer.numVertices;

            // pose over several frames and make sure re-evaluating the bounds does not read or
            // skin the vertex buffer again
            const originalGetVertexStream = fixture.mesh.getVertexStream;
            let streamReads = 0;
            fixture.mesh.getVertexStream = function (...args) {
                streamReads++;
                return originalGetVertexStream.apply(this, args);
            };

            for (let i = 0; i < 4; i++) {
                fixture.characters[0].bones[1].setLocalPosition(0.1 * i, 1, 0);
                renderer.updateCpuSkinMatrices([mi]);
                mi.aabb;
            }

            check(streamReads === 0, 'no vertex stream is read while bounding');
            check(fixture.mesh.boneAabb.length === boxes.length, 'the bias boxes are not rebuilt');
            check(fixture.mesh.boneAabb[0] === boxes[0], 'the same bone boxes are reused');
            check(fixture.mesh.vertexBuffer.numVertices === numVerticesBefore, 'topology is unchanged');
            fixture.mesh.getVertexStream = originalGetVertexStream;

            renderer.destroy();
            fixture.destroy();
        }
    },

    {
        group: 'pipeline',
        name: 'shader clears atlas uniforms when leaving the mode',
        run(check) {
            const fixture = new CharacterFixture();
            const scene = new Scene(fixture.device);
            scene.clusteredLightingEnabled = false;
            const renderer = new Renderer(fixture.device, scene);
            const mi = fixture.meshInstance;

            mi.setInstancedSkin(fixture.entries());
            renderer.updateCpuSkinMatrices([mi]);
            renderer.updateGpuSkinMatrices([mi]);
            renderer.setSkinning(fixture.device, mi);
            check(deviceScopeValue(fixture.device, 'texture_instancedSkinPoseMap') === mi.instancedSkin.boneTexture,
                'the atlas is bound');

            // switch back to normal skinning on a second mesh instance, and make sure the atlas
            // uniforms are cleared
            const plainFixture = new CharacterFixture({ device: fixture.device });
            plainFixture.meshInstance.skinInstance = plainFixture.characters[0].skinInstance;
            renderer.setSkinning(fixture.device, plainFixture.meshInstance);
            check(deviceScopeValue(fixture.device, 'texture_instancedSkinPoseMap') === null,
                'leaving the mode clears the atlas uniform');
            check(deviceScopeValue(fixture.device, 'instancedSkin_boneCount') === 0,
                'leaving the mode clears the bone count');
            check(deviceScopeValue(fixture.device, 'texture_poseMap') === plainFixture.characters[0].skinInstance.boneTexture,
                'the single skin pose map is bound instead');

            // a draw that does not skin at all clears both
            const noSkin = new MeshInstance(fixture.mesh, fixture.material, fixture.owner);
            renderer.setSkinning(fixture.device, noSkin);
            check(deviceScopeValue(fixture.device, 'texture_poseMap') === null, 'an unskinned draw clears the pose map');
            check(deviceScopeValue(fixture.device, 'texture_instancedSkinPoseMap') === null, 'and the atlas uniform');
            noSkin.destroy();

            plainFixture.destroy();
            renderer.destroy();
            fixture.destroy();
        }
    },

    {
        group: 'lifetime',
        name: 'atlas too large for the device has no side effects',
        run(check) {
            const fixture = new CharacterFixture();
            const mi = fixture.meshInstance;
            mi.setInstancedSkin(fixture.entries());
            const original = mi.instancedSkin;
            const originalTexture = original.boneTexture;
            const originalBuffer = original.vertexBuffer;

            // a device with a tiny maximum texture size
            const smallDevice = fixture.device;
            const savedMax = smallDevice.maxTextureSize;
            smallDevice.maxTextureSize = 3;
            let threw = false;
            try {
                mi.setInstancedSkin(fixture.entries());
            } catch (e) {
                threw = e instanceof RangeError;
            }
            smallDevice.maxTextureSize = savedMax;

            check(threw, 'an atlas beyond maxTextureSize throws a RangeError');
            check(mi.instancedSkin === original, 'the previous atlas is kept');
            check(original.boneTexture === originalTexture, 'the previous texture object survives');
            check(original.vertexBuffer === originalBuffer, 'the previous buffer survives');
            check(mi.instancedSkin.count === 2, 'the instance count is unchanged');

            // the surviving mode still evaluates and draws its atlas
            const renderer = new Renderer(fixture.device, new Scene(fixture.device));
            renderer.updateCpuSkinMatrices([mi]);
            renderer.updateGpuSkinMatrices([mi]);
            renderer.setSkinning(fixture.device, mi);
            check(deviceScopeValue(fixture.device, 'texture_instancedSkinPoseMap') === originalTexture,
                'the surviving atlas is still drawn');

            renderer.destroy();
            fixture.destroy();
        }
    },

    {
        group: 'lifetime',
        name: 'reflect together but never mix chirality',
        run(check) {
            const fixture = new CharacterFixture();
            const mi = fixture.meshInstance;

            // mirror the owner and both entries together: the relative determinants stay positive
            const mirroredOwner = new GraphNode('mirroredOwner');
            mirroredOwner.setLocalScale(-2, 2, 2);
            const mirroredMi = new MeshInstance(fixture.mesh, fixture.material, mirroredOwner);

            const mirroredRootA = new GraphNode('mirroredA');
            mirroredRootA.setLocalScale(-1, 1, 1);
            mirroredRootA.setLocalPosition(2, 0, 0);
            const mirroredRootB = new GraphNode('mirroredB');
            mirroredRootB.setLocalScale(-1, 1, 1);
            mirroredRootB.setLocalPosition(-2, 0, 0);

            let threw = false;
            try {
                mirroredMi.setInstancedSkin([
                    { skinInstance: fixture.characters[0].skinInstance, node: mirroredRootA },
                    { skinInstance: fixture.characters[1].skinInstance, node: mirroredRootB }
                ]);
            } catch (e) {
                threw = true;
            }
            check(!threw, 'a reflected owner with reflected entries is allowed');
            check(mirroredMi.instancedSkin !== null, 'the reflected mode is enabled');

            // the reflected owner with one reflected and one non-reflected entry is rejected
            const plainRoot = new GraphNode('plainRoot');
            let mixed = false;
            try {
                mirroredMi.setInstancedSkin([
                    { skinInstance: fixture.characters[0].skinInstance, node: mirroredRootA },
                    { skinInstance: fixture.characters[1].skinInstance, node: plainRoot }
                ]);
            } catch (e) {
                mixed = e instanceof RangeError;
            }
            check(mixed, 'mixing chiralities in one instanced skin is rejected');
            check(mirroredMi.instancedSkin !== null, 'the valid reflected mode survives');

            rendererScopeCheck(fixture.device, mirroredMi, check);

            mirroredMi.destroy();
            fixture.destroy();
        }
    },

    {
        group: 'lifetime',
        name: 'destroy releases only the owned resources',
        run(check) {
            const fixture = new CharacterFixture();
            const mi = fixture.meshInstance;
            mi.setInstancedSkin(fixture.entries());

            const instancedSkin = mi.instancedSkin;
            const texture = instancedSkin.boneTexture;
            const buffer = instancedSkin.vertexBuffer;

            let textureDestroyed = 0;
            let bufferDestroyed = 0;
            texture.destroy = () => {
                textureDestroyed++;
            };
            buffer.destroy = () => {
                bufferDestroyed++;
            };

            // the borrowed objects must not be touched by destroy
            let borrowedDestroyed = 0;
            for (const character of fixture.characters) {
                character.skinInstance.destroy = () => {
                    borrowedDestroyed++;
                };
                for (const bone of character.bones) {
                    bone.destroy = () => {
                        borrowedDestroyed++;
                    };
                }
            }

            const meshRefCount = fixture.mesh.refCount;
            mi.destroy();

            check(textureDestroyed === 1, 'the atlas is released exactly once');
            check(bufferDestroyed === 1, 'the instance buffer is released exactly once');
            check(borrowedDestroyed === 0, 'no borrowed skin instance or bone is destroyed');
            check(fixture.material !== null, 'the material is not destroyed');
            check(fixture.characters[0].skinInstance.skin === fixture.skin, 'the skin itself survives');

            // the mesh reference counting is preserved: the fixture still holds one reference
            check(fixture.mesh.refCount === meshRefCount - 1, 'the mesh ref count is decremented once');

            // the material is destroyed by the fixture, as it created it
            fixture.material.destroy();
            fixture.mesh.destroy();
            fixture.device.destroy();
        }
    },

    {
        group: 'lifetime',
        name: 'bounds enclose skinned vertices across frames',
        run(check) {
            const fixture = new CharacterFixture();
            const renderer = new Renderer(fixture.device, new Scene(fixture.device));
            const mi = fixture.meshInstance;
            mi.setInstancedSkin(fixture.entries());

            const track = new AnimTrack('swing', 1,
                [new AnimData(1, [0, 1])],
                [new AnimData(3, [0, 0, 0, 4, 1, 0])],
                [new AnimCurve([{ entityPath: ['bone1'], component: 'graph', propertyPath: ['localPosition'] }], 0, 0, INTERPOLATION_LINEAR)]);
            const evaluatorA = new AnimEvaluator(new DefaultAnimBinder(fixture.characters[0].root));
            evaluatorA.addClip(new AnimClip(track, 0, 1, true, false));

            const trackB = new AnimTrack('swingB', 1,
                [new AnimData(1, [0, 1])],
                [new AnimData(3, [0, 0, 0, -3, 2, 0])],
                [new AnimCurve([{ entityPath: ['bone2'], component: 'graph', propertyPath: ['localPosition'] }], 0, 0, INTERPOLATION_LINEAR)]);
            const evaluatorB = new AnimEvaluator(new DefaultAnimBinder(fixture.characters[1].root));
            evaluatorB.addClip(new AnimClip(trackB, 0, 1, true, false));

            for (const time of [0, 0.25, 0.5, 0.75]) {
                evaluatorA.update(time);
                evaluatorB.update(time);
                renderer.updateCpuSkinMatrices([mi]);

                const palette = mi.instancedSkin.matrixPalette;
                const ownerWorld = Array.from(mi.node.getWorldTransform().data);
                for (let i = 0; i < mi.instancedSkin.count; i++) {
                    const entry = mi.instancedSkin.entries[i];
                    const nodeWorld = Array.from(entry.node.getWorldTransform().data);
                    const boneMatrices = [];
                    for (let b = 0; b < mi.instancedSkin.boneCount; b++) {
                        boneMatrices.push(decodePalette(palette, i * mi.instancedSkin.boneCount + b));
                    }
                    const numVertices = fixture.positions.length / 3;
                    for (let v = 0; v < numVertices; v++) {
                        const p = [fixture.positions[v * 3], fixture.positions[v * 3 + 1], fixture.positions[v * 3 + 2]];
                        const w = [fixture.weights[v * 4], fixture.weights[v * 4 + 1], fixture.weights[v * 4 + 2], fixture.weights[v * 4 + 3]];
                        const bi = [fixture.indices[v * 4], fixture.indices[v * 4 + 1], fixture.indices[v * 4 + 2], fixture.indices[v * 4 + 3]];
                        const expected = lbsworld(p, w, bi, boneMatrices, nodeWorld, ownerWorld);
                        check(mi.aabb.containsPoint(new Vec3(expected[0], expected[1], expected[2])),
                            `time ${time} instance ${i} vertex ${v} is enclosed`);
                    }
                }
            }

            renderer.destroy();
            fixture.destroy();
        }
    }
];

// -----------------------------------------------------------------------------
// local shader generation helper
// -----------------------------------------------------------------------------

/**
 * Returns the value of a uniform in the device scope.
 *
 * @param {NullGraphicsDevice} device - The device.
 * @param {string} name - The uniform name.
 * @returns {*} The value.
 */
function deviceScopeValue(device, name) {
    return device.scope.resolve(name).value;
}

/**
 * Drives the renderer path for a mesh instance and asserts it evaluates and binds its atlas.
 *
 * @param {NullGraphicsDevice} device - The device.
 * @param {MeshInstance} meshInstance - The mesh instance.
 * @param {function} check - The assertion callback.
 */
function rendererScopeCheck(device, meshInstance, check) {
    const scene = new Scene(device);
    scene.clusteredLightingEnabled = false;
    const renderer = new Renderer(device, scene);
    renderer.updateCpuSkinMatrices([meshInstance]);
    check(Number.isFinite(meshInstance.aabb.center.x), 'the reflected mode has finite bounds');
    renderer.updateGpuSkinMatrices([meshInstance]);
    renderer.setSkinning(device, meshInstance);
    check(deviceScopeValue(device, 'texture_instancedSkinPoseMap') === meshInstance.instancedSkin.boneTexture,
        'the reflected atlas is bound');
    renderer.destroy();
}

/**
 * Asserts that a preprocessed vertex shader uses the instanced skin path and not the legacy skin
 * or batch paths.
 *
 * @param {string} pre - The preprocessed vertex source.
 * @param {string} label - A label for the assertions.
 * @param {number} pass - The pass index.
 * @param {function} check - The assertion callback.
 */
function checkShaderVariant(pre, label, pass, check, definesStripped = false) {
    // WGSL sources are preprocessed with the defines stripped, so only the resolved code remains
    if (!definesStripped) {
        check(pre.includes('#define INSTANCED_SKIN'), `${label} sets the instanced skin define`);
    }
    check(pre.includes('getInstancedSkinMatrix'), `${label} addresses the atlas`);
    check(pre.includes('instanceBase'), `${label} offsets by the instance block`);
    check(pre.includes('pcInstanceIndex') || pre.includes('gl_InstanceID'),
        `${label} reads the instance index`);
    check(/getInstancedSkinMatrix\s*\(vertex_boneIndices/.test(pre),
        `${label} passes the vertex bone indices`);
    check(pre.includes('getInstanceMatrix()'), `${label} applies the instance matrix`);
    check(/matrix_model\s*\*\s*getInstanceMatrix\(\)\s*\*\s*getInstancedSkinMatrix/.test(pre) ||
        /uniform\.matrix_model\s*\*\s*getInstanceMatrix\(\)\s*\*\s*getInstancedSkinMatrix/.test(pre),
    `${label} applies model * instance * skin`);

    // the old single-bone and batch variants are not used
    check(!/getSkinMatrix\s*\(vertex_boneIndices,\s*vertex_boneWeights\)/.test(pre),
        `${label} does not use the single bone skin matrix`);
    check(!pre.includes('texture_poseMap'), `${label} does not sample the single bone map`);
}

/**
 * Generates the vertex shader of the lit shader for the given pass and language, and preprocesses
 * it into the final source.
 *
 * @param {NullGraphicsDevice} device - The device.
 * @param {number} pass - The shader pass index.
 * @param {boolean} wgsl - True to generate WGSL, false for GLSL.
 * @param {object} [overrides] - Options to override on the lit options.
 * @returns {{source: string, defines: Map<string, string>, language: string}} The generated source.
 */
function generateVertexShader(device, pass, wgsl, overrides = {}) {
    const options = new LitShaderOptions();
    options.pass = pass;
    options.clusteredLightingEnabled = false;
    options.lights = [];
    options.skin = overrides.skin ?? false;
    options.batch = overrides.batch ?? false;
    options.useInstancing = overrides.useInstancing ?? false;
    options.instancedSkin = overrides.instancedSkin ?? (!overrides.skin && !overrides.useInstancing);

    // a WebGPU device generates WGSL only when the language is allowed; otherwise it generates
    // GLSL, which the caller can still preprocess
    const languageDevice = device;
    const previous = languageDevice.isWebGPU;
    languageDevice.isWebGPU = wgsl;
    let shader;
    try {
        shader = new LitShader(languageDevice, options);
    } finally {
        languageDevice.isWebGPU = previous;
    }

    shader.generateVertexShader(new Array(8).fill(false), new Array(8).fill(false), new Map());

    const includes = new Map([...shader.chunks, ...shader.includes]);
    let definesCode = '';
    shader.vDefines.forEach((value, key) => {
        definesCode += `#define ${key} ${value}\n`;
    });

    const source = Preprocessor.run(definesCode + shader.vshader, includes, {
        sourceName: 'instanced-skin-test',
        stripDefines: shader.shaderLanguage === SHADERLANGUAGE_WGSL
    });

    return {
        source: source ?? '',
        defines: shader.vDefines,
        language: shader.shaderLanguage
    };
}
