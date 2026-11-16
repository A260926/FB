// Instanced skin example - several live skeletons drawn with one instanced draw call.
//
// This example runs without a browser or a GPU. It builds one skinned mesh, animates two
// characters with different tracks, hands both to a MeshInstance through setInstancedSkin, and
// runs the real Renderer path (pre-cull CPU pose update, GPU pose upload, skinning bind). It then
// decodes the world space vertex positions from the pose atlas and the instance buffer the
// renderer produced, and writes them to `poses.json` in the output directory.
//
// Usage: node examples/instanced-skin.mjs <output-directory>
//
// The output has the shape:
//
//     { "frames": [ { "time": 0.25, "instances": [ { "positions": [x, y, z, ...] }, ... ] } ] }
//
// The positions are produced by the production atlas and instance buffer, not by a separate
// animation path: the same 12 float atlas rows and per-instance matrices the shader reads.

import fs from 'node:fs';
import path from 'node:path';

import { Mat4 } from '../src/core/math/mat4.js';
import { Vec3 } from '../src/core/math/vec3.js';
import { NullGraphicsDevice } from '../src/platform/graphics/null/null-graphics-device.js';
import {
    SEMANTIC_BLENDINDICES,
    SEMANTIC_BLENDWEIGHT,
    TYPE_UINT8
} from '../src/platform/graphics/constants.js';
import { GraphNode } from '../src/scene/graph-node.js';
import { Mesh } from '../src/scene/mesh.js';
import { MeshInstance } from '../src/scene/mesh-instance.js';
import { Scene } from '../src/scene/scene.js';
import { Skin } from '../src/scene/skin.js';
import { SkinInstance } from '../src/scene/skin-instance.js';
import { StandardMaterial } from '../src/scene/materials/standard-material.js';
import { Renderer } from '../src/scene/renderer/renderer.js';
import { AnimData } from '../src/framework/anim/evaluator/anim-data.js';
import { AnimTrack } from '../src/framework/anim/evaluator/anim-track.js';
import { AnimCurve } from '../src/framework/anim/evaluator/anim-curve.js';
import { AnimClip } from '../src/framework/anim/evaluator/anim-clip.js';
import { AnimEvaluator } from '../src/framework/anim/evaluator/anim-evaluator.js';
import { DefaultAnimBinder } from '../src/framework/anim/binder/default-anim-binder.js';
import { INTERPOLATION_LINEAR } from '../src/framework/anim/constants.js';

// the geometry: a strip of vertices, each blended between two consecutive bones
const boneCount = 5;
const positions = [
    0.0, 0.0, 0.0,
    0.5, 0.0, 0.1,
    0.0, 1.0, 0.0,
    0.5, 1.0, 0.1,
    0.0, 2.0, 0.0,
    0.5, 2.0, 0.1,
    0.0, 3.0, 0.0,
    0.5, 3.0, 0.1
];
const vertexCount = positions.length / 3;
const weights = new Float32Array(vertexCount * 4);
const indices = new Uint8Array(vertexCount * 4);
for (let v = 0; v < vertexCount; v++) {
    const b0 = Math.min(v % boneCount, boneCount - 2);
    weights[v * 4] = 0.6;
    weights[v * 4 + 1] = 0.4;
    indices[v * 4] = b0;
    indices[v * 4 + 1] = b0 + 1;
    indices[v * 4 + 2] = b0;
    indices[v * 4 + 3] = b0 + 1;
}
const indexList = [0, 1, 2, 2, 1, 3, 2, 3, 4, 4, 3, 5, 4, 5, 6, 6, 5, 7];

/**
 * Builds one character: a root with a bone chain, and a skin instance bound to it.
 *
 * @param {Skin} skin - The shared skin.
 * @param {string} name - The name of the root node.
 * @param {Vec3} offset - The root's local position.
 * @param {number} phase - A per-character phase, so the two poses differ.
 * @returns {object} The character.
 */
function createCharacter(skin, name, offset, phase) {
    const root = new GraphNode(name);
    root.setLocalPosition(offset);
    root.setLocalEulerAngles(0, 20 + phase * 30, 0);

    const bones = [];
    for (let i = 0; i < boneCount; i++) {
        const bone = new GraphNode(`bone${i}`);
        bone.setLocalPosition(0.05 * Math.sin(phase * 3 + i), 1, 0);
        bone.setLocalEulerAngles(10 * phase, 5 * i, 3 * phase);
        (i === 0 ? root : bones[i - 1]).addChild(bone);
        bones.push(bone);
    }

    const skinInstance = new SkinInstance(skin);
    skinInstance.bones = bones;

    return { root, bones, skinInstance };
}

/**
 * Creates an animation track driving the local position of one bone of a character.
 *
 * @param {string} name - The track name.
 * @param {string} bonePath - The bone name.
 * @param {number} amplitude - The movement amplitude.
 * @returns {AnimTrack} The track.
 */
function createTrack(name, bonePath, amplitude) {
    return new AnimTrack(name, 1,
        [new AnimData(1, [0, 1])],
        [new AnimData(3, [0, 0, 0, amplitude, 0, 0])],
        [new AnimCurve([{ entityPath: [bonePath], component: 'graph', propertyPath: ['localPosition'] }], 0, 0, INTERPOLATION_LINEAR)]
    );
}

/**
 * Decodes the world space position of a vertex from the pose atlas and the instance buffer,
 * exactly as the vertex shader would transform it.
 *
 * @param {Float32Array} palette - The pose atlas CPU data.
 * @param {Float32Array|number[]} instanceMatrix - The column-major instance matrix of the entry.
 * @param {number[]} ownerWorld - The column-major world matrix of the draw owner.
 * @param {number} instanceIndex - The index of the entry.
 * @param {number} boneCount - The number of bones of one entry.
 * @param {number} vertexIndex - The vertex index.
 * @returns {number[]} The world space position.
 */
function decodeVertex(palette, instanceMatrix, ownerWorld, instanceIndex, boneCount, vertexIndex) {
    // blend the node-space skin matrices of the four bones of the vertex
    const p = [positions[vertexIndex * 3], positions[vertexIndex * 3 + 1], positions[vertexIndex * 3 + 2]];
    const blended = [0, 0, 0];
    for (let k = 0; k < 4; k++) {
        const weight = weights[vertexIndex * 4 + k];
        if (weight === 0) continue;
        const bone = indices[vertexIndex * 4 + k];

        // the 12 float row layout of the atlas, transposed back to a 4x4 matrix
        const base = (instanceIndex * boneCount + bone) * 12;
        const v1 = [palette[base], palette[base + 1], palette[base + 2], palette[base + 3]];
        const v2 = [palette[base + 4], palette[base + 5], palette[base + 6], palette[base + 7]];
        const v3 = [palette[base + 8], palette[base + 9], palette[base + 10], palette[base + 11]];
        const row = [
            v1[0], v2[0], v3[0], 0,
            v1[1], v2[1], v3[1], 0,
            v1[2], v2[2], v3[2], 0,
            v1[3], v2[3], v3[3], 1
        ];
        for (let r = 0; r < 3; r++) {
            blended[r] += weight * (row[r] * p[0] + row[4 + r] * p[1] + row[8 + r] * p[2] + row[12 + r]);
        }
    }

    // model * instance * skinned vertex, with every product done on plain arrays
    const instancePos = [0, 0, 0];
    for (let r = 0; r < 3; r++) {
        instancePos[r] = instanceMatrix[r] * blended[0] + instanceMatrix[4 + r] * blended[1] +
            instanceMatrix[8 + r] * blended[2] + instanceMatrix[12 + r];
    }
    const out = [0, 0, 0];
    for (let r = 0; r < 3; r++) {
        out[r] = ownerWorld[r] * instancePos[0] + ownerWorld[4 + r] * instancePos[1] +
            ownerWorld[8 + r] * instancePos[2] + ownerWorld[12 + r];
    }
    return out;
}

/**
 * Runs the example.
 *
 * @param {string} outDir - The output directory.
 */
async function main(outDir) {

    if (!outDir) {
        throw new Error('Usage: node examples/instanced-skin.mjs <output-directory>');
    }
    fs.mkdirSync(outDir, { recursive: true });

    // a headless device - no GPU is used, but all the production classes run
    const device = new NullGraphicsDevice();

    // the mesh, its skin and its material
    const mesh = new Mesh(device);
    mesh.setPositions(positions);
    mesh.setVertexStream(SEMANTIC_BLENDINDICES, indices, 4, vertexCount, TYPE_UINT8);
    mesh.setVertexStream(SEMANTIC_BLENDWEIGHT, weights, 4);
    mesh.setIndices(indexList);
    mesh.update();

    const inverseBindPose = [];
    for (let i = 0; i < boneCount; i++) {
        inverseBindPose.push(new Mat4().setTranslate(0, -i, 0));
    }
    const skin = new Skin(device, inverseBindPose, inverseBindPose.map((_, i) => `bone${i}`));
    mesh.skin = skin;

    // the draw owner and the two characters
    const owner = new GraphNode('owner');
    owner.setLocalPosition(0, 0, 0);

    const characterA = createCharacter(skin, 'characterA', new Vec3(-1.5, 0, 0), 0.25);
    const characterB = createCharacter(skin, 'characterB', new Vec3(1.5, 0.5, 0), 0.75);

    // the mesh instance, drawing both characters with one instanced draw
    const material = new StandardMaterial();
    const meshInstance = new MeshInstance(mesh, material, owner);
    meshInstance.setInstancedSkin([
        { skinInstance: characterA.skinInstance, node: characterA.root },
        { skinInstance: characterB.skinInstance, node: characterB.root }
    ]);

    // the renderer, with the clustered lighting disabled as there is no light
    const scene = new Scene(device);
    scene.clusteredLightingEnabled = false;
    const renderer = new Renderer(device, scene);

    // an animation per character
    const evaluatorA = new AnimEvaluator(new DefaultAnimBinder(characterA.root));
    evaluatorA.addClip(new AnimClip(createTrack('swingA', 'bone1', 2), 0, 1, true, false));
    const evaluatorB = new AnimEvaluator(new DefaultAnimBinder(characterB.root));
    evaluatorB.addClip(new AnimClip(createTrack('swingB', 'bone2', -1.5), 0, 1, true, false));

    const instancedSkin = meshInstance.instancedSkin;
    const boneCountPerInstance = instancedSkin.boneCount;

    const frames = [];
    const times = [0, 0.25, 0.5, 0.75];

    for (const time of times) {

        // sample both animations
        evaluatorA.update(time);
        evaluatorB.update(time);

        // the renderer evaluates the pose and the bounds before culling, then uploads the same
        // result and binds the atlas for the draw
        renderer.updateCpuSkinMatrices([meshInstance]);
        renderer.updateGpuSkinMatrices([meshInstance]);
        renderer.setSkinning(device, meshInstance);

        // decode the world positions of both characters from the production atlas and the
        // instance buffer
        const palette = instancedSkin.matrixPalette;
        const ownerWorld = Array.from(meshInstance.node.getWorldTransform().data);

        // the instance matrices the renderer uploaded
        const storage = instancedSkin.vertexBuffer.lock();
        const instanceData = ArrayBuffer.isView(storage) ?
            new Float32Array(storage.buffer, storage.byteOffset, storage.byteLength / 4) :
            new Float32Array(storage);

        const instances = [];

        for (let i = 0; i < instancedSkin.count; i++) {

            const instanceMatrix = instanceData.subarray(i * 16, i * 16 + 16);

            const worldPositions = [];
            for (let v = 0; v < vertexCount; v++) {
                // the attribute streams are the same for every instance
                const p = decodeVertex(palette, instanceMatrix, ownerWorld, i, boneCountPerInstance, v);
                worldPositions.push(p[0], p[1], p[2]);
            }

            instances.push({ positions: worldPositions });
        }

        frames.push({ time, instances });
    }

    const result = { frames };
    fs.writeFileSync(path.join(outDir, 'poses.json'), JSON.stringify(result));

    // report the bounds of the last frame as a sanity check
    const lastFrame = frames[frames.length - 1];
    const first = lastFrame.instances[0].positions;
    console.log(`INSTANCED_SKIN_EXAMPLE frames=${frames.length} instances=${lastFrame.instances.length} ` +
        `vertices=${first.length / 3} bones=${boneCountPerInstance} first=${first[0].toFixed(3)},${first[1].toFixed(3)}`);

    // release the resources this example owns
    renderer.destroy();
    meshInstance.destroy();
    material.destroy();
    mesh.destroy();
    device.destroy();
}

main(process.argv[2]).catch((err) => {
    console.error(err);
    process.exit(1);
});
