import { Debug } from '../core/debug.js';
import { math } from '../core/math/math.js';
import { Mat4 } from '../core/math/mat4.js';
import { BoundingBox } from '../core/shape/bounding-box.js';
import {
    BUFFER_DYNAMIC,
    FILTER_NEAREST,
    PIXELFORMAT_RGBA32F,
    PRIMITIVE_TRIANGLES,
    SEMANTIC_BLENDINDICES,
    SEMANTIC_BLENDWEIGHT,
    SEMANTIC_POSITION,
    TEXTURELOCK_READ
} from '../platform/graphics/constants.js';
import { Texture } from '../platform/graphics/texture.js';
import { VertexBuffer } from '../platform/graphics/vertex-buffer.js';
import { VertexFormat } from '../platform/graphics/vertex-format.js';
import { VertexIterator } from '../platform/graphics/vertex-iterator.js';
import { standardMaterialTextureParameters } from './materials/standard-material-parameters.js';
import { StandardMaterial } from './materials/standard-material.js';
import { SkinInstance } from './skin-instance.js';

/**
 * @import { GraphicsDevice } from '../platform/graphics/graphics-device.js'
 * @import { GraphNode } from './graph-node.js'
 * @import { Mesh } from './mesh.js'
 * @import { MeshInstance } from './mesh-instance.js'
 */

// the sum of the weights of a vertex may differ from 1 by this much
const WEIGHT_SUM_EPSILON = 1e-5;

// matrices reused by the per-frame evaluation, never exported
const _invOwner = new Mat4();
const _invNode = new Mat4();
const _instanceMatrix = new Mat4();
const _skinMatrix = new Mat4();
const _relative = new Mat4();
const _boneAabb = new BoundingBox();
const _instanceAabb = new BoundingBox();

/**
 * Returns the determinant of the linear (upper left 3x3) part of a matrix.
 *
 * @param {Mat4} m - The matrix.
 * @returns {number} The determinant.
 */
const determinant3 = (m) => {
    const d = m.data;
    return d[0] * (d[5] * d[10] - d[9] * d[6]) -
        d[4] * (d[1] * d[10] - d[9] * d[2]) +
        d[8] * (d[1] * d[6] - d[5] * d[2]);
};

/**
 * Returns true when all 16 elements of a matrix are finite.
 *
 * @param {Mat4} m - The matrix.
 * @returns {boolean} True when finite.
 */
const isFiniteMatrix = (m) => {
    const d = m.data;
    for (let i = 0; i < 16; i++) {
        if (!Number.isFinite(d[i])) {
            return false;
        }
    }
    return true;
};

/**
 * Throws a RangeError for an unsupported configuration of the instanced skin path, rather than
 * silently ignoring it.
 *
 * @param {string} message - The error message.
 * @ignore
 */
const reject = (message) => {
    throw new RangeError(`InstancedSkin: ${message}`);
};

/**
 * A pose atlas and instance matrix buffer for drawing multiple live skeletons with a single
 * instanced draw call, see {@link MeshInstance#setInstancedSkin}.
 *
 * The atlas stores the bone matrices of every entry consecutively, by instance and then by bone,
 * using the same 12 float row layout as {@link SkinInstance#matrixPalette}: each bone occupies
 * three RGBA32F texels, and the atlas width is a multiple of three so that no bone's texels cross
 * a row. The instance buffer holds one column-major 4x4 matrix per entry, in the layout of
 * {@link VertexFormat.getDefaultInstancingFormat}.
 *
 * Each entry brings its own {@link SkinInstance} and the node that is the coordinate root of that
 * copy of the mesh. The shader receives:
 *
 * `bone matrix = inverse(node.world) * bone.world * inverseBind`
 *
 * `instance matrix = inverse(meshInstance.node.world) * node.world`
 *
 * so the model matrix, the instance matrix and the skin matrix apply the world transform exactly
 * once.
 *
 * The class is created by {@link MeshInstance#setInstancedSkin} and is not constructed directly.
 * It borrows the skin instances and their bones: it never rewrites their matrices, matrix palette,
 * frame state or resources. It owns only the atlas texture and the instance vertex buffer.
 *
 * @category Graphics
 */
class InstancedSkin {
    /**
     * The graphics device.
     *
     * @type {GraphicsDevice}
     * @private
     */
    _device;

    /**
     * The borrowed entries, in draw order, copied out of the caller's array.
     *
     * @type {{skinInstance: SkinInstance, node: GraphNode}[]}
     * @private
     */
    _entries = [];

    /**
     * The number of bones of each entry, the same for all of them.
     *
     * @type {number}
     * @private
     */
    _boneCount = 0;

    /**
     * The mesh the entries borrow, used for its bone influence boxes.
     *
     * @type {Mesh|null}
     * @private
     */
    _mesh = null;

    /**
     * The world space bounds of all blended vertices, as last evaluated.
     *
     * @type {BoundingBox}
     * @private
     */
    _aabb = new BoundingBox();

    /**
     * True when {@link InstancedSkin#_aabb} was evaluated since the last change.
     *
     * @type {boolean}
     * @private
     */
    _aabbValid = false;

    /**
     * A signature of the transform versions the last evaluation read. The bounds are re-evaluated
     * when the current transforms no longer match it.
     *
     * @type {number}
     * @private
     */
    _signature = 0;

    /**
     * The atlas texture, or null when there are no entries.
     *
     * @type {Texture|null}
     * @private
     */
    _boneTexture = null;

    /**
     * The CPU copy of the atlas, including the padding at the end of the texture. Null when there
     * are no entries.
     *
     * @type {Float32Array|null}
     * @private
     */
    _matrixPalette = null;

    /**
     * The per-instance matrix vertex buffer, or null when there are no entries.
     *
     * @type {VertexBuffer|null}
     * @private
     */
    _vertexBuffer = null;

    /**
     * The CPU copy of the instance matrices, or null when there are no entries.
     *
     * @type {Float32Array|null}
     * @private
     */
    _instanceData = null;

    /**
     * True when the CPU data changed since the last upload.
     *
     * @type {boolean}
     * @private
     */
    _dirty = false;

    /**
     * Creates an instanced skin. Callers go through {@link MeshInstance#setInstancedSkin}, which
     * validates the entries before calling this constructor.
     *
     * @param {GraphicsDevice} device - The graphics device.
     * @param {Mesh} mesh - The mesh the entries borrow.
     * @param {{skinInstance: SkinInstance, node: GraphNode}[]} entries - The validated entries.
     * @param {number} boneCount - The number of bones of every entry.
     * @private
     */
    constructor(device, mesh, entries, boneCount) {
        this._device = device;
        this._mesh = mesh;
        this._entries = entries;
        this._boneCount = boneCount;
        this._aabb.center.set(0, 0, 0);
        this._aabb.halfExtents.set(0, 0, 0);

        if (entries.length > 0) {
            this._prepareBoneBounds(mesh);
            this._createResources();
        }
    }

    /**
     * The number of bones of each character in the atlas. Zero when the instanced skin has no
     * entries.
     *
     * @type {number}
     */
    get boneCount() {
        return this._boneCount;
    }

    /**
     * The number of character instances drawn in one instanced draw call.
     *
     * @type {number}
     */
    get count() {
        return this._entries.length;
    }

    /**
     * The atlas texture, storing one 4x3 bone matrix per bone and instance, or null when the
     * instanced skin has no entries. Owned by this instance and destroyed with it.
     *
     * @type {Texture|null}
     */
    get boneTexture() {
        return this._boneTexture;
    }

    /**
     * The CPU copy of the atlas, including any padding at the end of the texture, for debugging.
     * Owned by this instance and do not modify it.
     *
     * @type {Float32Array|null}
     */
    get matrixPalette() {
        return this._matrixPalette;
    }

    /**
     * The world space bounding box of the blended vertices of all entries, as last evaluated. It
     * is derived from the mesh bone influence boxes, not from per-vertex skinning.
     *
     * @type {BoundingBox}
     */
    get aabb() {
        return this._aabb;
    }

    /**
     * The per-instance matrix vertex buffer, or null when the instanced skin has no entries. Owned
     * by this instance and destroyed with it.
     *
     * @type {VertexBuffer|null}
     * @ignore
     */
    get vertexBuffer() {
        return this._vertexBuffer;
    }

    /**
     * The borrowed entries, in draw order. Do not modify.
     *
     * @type {{skinInstance: SkinInstance, node: GraphNode}[]}
     * @ignore
     */
    get entries() {
        return this._entries;
    }

    /**
     * The GPU data version, incremented when the CPU data changed since the last upload.
     *
     * @ignore
     */
    get dirty() {
        return this._dirty;
    }

    /**
     * Allocates the atlas texture and the instance matrix buffer. Runs after the texture size has
     * been validated.
     *
     * @private
     */
    _createResources() {
        const device = this._device;
        const count = this._entries.length;
        const boneCount = this._boneCount;

        // the atlas width is a multiple of 3, so no bone's three texels cross a row
        const numPixels = count * boneCount * 3;
        const maxSize = device.maxTextureSize ?? 4096;
        const maxWidth = Math.max(3, Math.floor(maxSize / 3) * 3);
        const idealWidth = math.roundUp(Math.ceil(Math.sqrt(numPixels)), 3);
        const width = Math.max(3, Math.min(idealWidth, maxWidth));
        const height = Math.ceil(numPixels / width);

        if (width > maxSize || height > maxSize) {
            reject(`the pose atlas for ${count} instance(s) of ${boneCount} bones (${width} x ${height}) exceeds the device maximum texture size of ${maxSize}.`);
        }

        this._boneTexture = new Texture(device, {
            width: width,
            height: height,
            format: PIXELFORMAT_RGBA32F,
            mipmaps: false,
            minFilter: FILTER_NEAREST,
            magFilter: FILTER_NEAREST,
            name: 'instancedSkin'
        });

        // the same CPU view as SkinInstance uses, including the tail padding of the texture
        this._matrixPalette = this._boneTexture.lock({ mode: TEXTURELOCK_READ });
        this._boneTexture.unlock();

        // one matrix per instance, in the default instancing layout
        const format = VertexFormat.getDefaultInstancingFormat(device);
        format.instancing = true;
        this._instanceData = new Float32Array(count * 16);
        this._vertexBuffer = new VertexBuffer(device, format, count, {
            usage: BUFFER_DYNAMIC,
            data: this._instanceData
        });
    }

    /**
     * Prepares, once, the data used to bound the skinned vertices: the influence box of each bone
     * in mesh space, computed from the mesh topology. Per-frame updates transform these boxes by
     * the current bone matrices, so no vertex is ever skinned on the CPU to find the bounds.
     *
     * @param {Mesh} mesh - The mesh.
     * @private
     */
    _prepareBoneBounds(mesh) {
        if (!mesh.boneAabb) {
            mesh._initBoneAabbs(null);
        }
    }

    /**
     * Evaluates the bone matrices, the instance matrices and the world bounding box of every entry
     * from the current node transforms. Called by the renderer before culling, so the pose, the
     * instance matrices and the bounds are consistent with what is uploaded and drawn.
     *
     * @param {MeshInstance} meshInstance - The owning mesh instance.
     * @ignore
     */
    update(meshInstance) {
        const entries = this._entries;
        this._aabbValid = true;

        if (entries.length === 0) {
            this._aabb.center.set(0, 0, 0);
            this._aabb.halfExtents.set(0, 0, 0);
            return;
        }

        const mesh = this._mesh;
        Debug.assert(mesh && this._matrixPalette && this._instanceData, 'an empty instanced skin does not need per-frame data');

        // re-evaluate the node transforms of the owner and all entries, so the frame is consistent
        _invOwner.copy(meshInstance.node.getWorldTransform()).invert();

        const boneUsed = mesh.boneUsed;
        const boneAabb = mesh.boneAabb;
        let anyBox = false;

        for (let i = 0; i < entries.length; i++) {
            const entry = entries[i];
            const skinInstance = entry.skinInstance;
            const nodeWorld = entry.node.getWorldTransform();
            const bones = skinInstance.bones;
            const inverseBindPose = skinInstance.skin.inverseBindPose;

            // the instance matrix is relative to the owner of the draw
            _instanceMatrix.mulAffine2(_invOwner, nodeWorld);
            this._instanceData.set(_instanceMatrix.data, i * 16);

            // the bone matrices are relative to the entry's own node
            _invNode.copy(nodeWorld).invert();

            const palette = this._matrixPalette;

            // a skinned vertex is a convex combination of its bones' skinned positions, each of
            // which lies in the world space box of the bone. The vertex therefore lies in the
            // convex hull of those boxes, which the union of the boxes encloses - so the union of
            // the bone influence boxes bounds this instance, without skinning any vertex.
            let instanceFirst = true;

            for (let b = 0; b < this._boneCount; b++) {
                const boneWorld = bones[b].getWorldTransform();

                _skinMatrix.mulAffine2(_invNode, boneWorld);
                _skinMatrix.mulAffine2(_skinMatrix, inverseBindPose[b]);

                const base = (i * this._boneCount + b) * 12;
                const pe = _skinMatrix.data;
                palette[base] = pe[0];
                palette[base + 1] = pe[4];
                palette[base + 2] = pe[8];
                palette[base + 3] = pe[12];
                palette[base + 4] = pe[1];
                palette[base + 5] = pe[5];
                palette[base + 6] = pe[9];
                palette[base + 7] = pe[13];
                palette[base + 8] = pe[2];
                palette[base + 9] = pe[6];
                palette[base + 10] = pe[10];
                palette[base + 11] = pe[14];

                if (boneUsed[b]) {
                    _skinMatrix.mulAffine2(boneWorld, inverseBindPose[b]);
                    _boneAabb.setFromTransformedAabb(boneAabb[b], _skinMatrix);

                    if (instanceFirst) {
                        instanceFirst = false;
                        _instanceAabb.center.copy(_boneAabb.center);
                        _instanceAabb.halfExtents.copy(_boneAabb.halfExtents);
                    } else {
                        _instanceAabb.add(_boneAabb);
                    }
                }
            }

            // union of the instance bounds
            if (!instanceFirst) {
                if (!anyBox) {
                    anyBox = true;
                    this._aabb.center.copy(_instanceAabb.center);
                    this._aabb.halfExtents.copy(_instanceAabb.halfExtents);
                } else {
                    this._aabb.add(_instanceAabb);
                }
            }
        }

        if (!anyBox) {
            this._aabb.center.set(0, 0, 0);
            this._aabb.halfExtents.set(0, 0, 0);
        }

        this._signature = this._transformSignature(meshInstance);
        this._dirty = true;
    }

    /**
     * Returns the world bounds of the blended vertices, evaluating them first if they were not
     * evaluated yet. Called by the renderer after {@link InstancedSkin#update}.
     *
     * @param {MeshInstance} meshInstance - The owning mesh instance.
     * @param {boolean} [evaluate=false] - True to force an evaluation when the bounds are stale.
     * @returns {BoundingBox} The world bounds.
     * @ignore
     */
    getAabb(meshInstance, evaluate = false) {
        if (this._entries.length > 0 &&
            (evaluate || !this._aabbValid || this._signature !== this._transformSignature(meshInstance))) {
            this.update(meshInstance);
        }
        return this._aabb;
    }

    /**
     * Returns a value that changes when any node transform the bounds depend on changed, so the
     * bounds are never stale when the renderer reads them.
     *
     * @param {MeshInstance} meshInstance - The owning mesh instance.
     * @returns {number} The signature.
     * @private
     */
    _transformSignature(meshInstance) {
        let signature = meshInstance.node?._aabbVer ?? 0;
        for (let i = 0; i < this._entries.length; i++) {
            const entry = this._entries[i];
            signature = (signature * 31 + (entry.node._aabbVer ?? 0)) | 0;
            const bones = entry.skinInstance.bones;
            for (let b = 0; b < bones.length; b++) {
                signature = (signature * 31 + (bones[b]._aabbVer ?? 0)) | 0;
            }
        }
        return signature;
    }

    /**
     * Uploads the CPU data evaluated by {@link InstancedSkin#update} to the GPU, once per change.
     *
     * @ignore
     */
    upload() {
        if (this._dirty) {
            this._dirty = false;
            this._boneTexture?.upload();
            this._vertexBuffer?.unlock();
        }
    }

    /**
     * Releases the resources owned by this instanced skin: only the atlas texture and the instance
     * vertex buffer. The borrowed skin instances, bones, mesh and material are not touched.
     */
    destroy() {
        this._boneTexture?.destroy();
        this._boneTexture = null;

        this._vertexBuffer?.destroy();
        this._vertexBuffer = null;

        this._matrixPalette = null;
        this._instanceData = null;
        this._entries = [];
        this._boneCount = 0;
        this._mesh = null;
        this._dirty = false;
        this._aabbValid = false;
    }

    /**
     * Validates the entries and, when they are valid, creates an instanced skin. Validation is
     * complete before any resource is allocated, so a failure leaves the caller's current
     * instanced skin, its resources and the render state untouched.
     *
     * @param {MeshInstance} meshInstance - The owning mesh instance.
     * @param {{skinInstance: SkinInstance, node: GraphNode}[]} entries - The borrowed entries.
     * @returns {InstancedSkin} The new instanced skin.
     * @ignore
     */
    static create(meshInstance, entries) {

        // the mode cannot be combined with the older render state
        if (!Array.isArray(entries)) {
            reject('entries must be an array, or null to disable the mode.');
        }
        if (!meshInstance.mesh) {
            reject('the mesh instance has no mesh.');
        }
        if (meshInstance.skinInstance) {
            reject('a plain skinInstance is already assigned; clear it before enabling instanced skin.');
        }
        if (meshInstance.morphInstance) {
            reject('a morphInstance is already assigned; clear it before enabling instanced skin.');
        }
        if (meshInstance.instancingData) {
            reject('custom hardware instancing is already assigned; clear it before enabling instanced skin.');
        }
        if (meshInstance.drawCommands) {
            reject('indirect drawing and multi-draw are not supported by instanced skin.');
        }
        if (meshInstance.batching) {
            reject('batching is not supported by instanced skin.');
        }

        const material = meshInstance.material;
        if (!(material instanceof StandardMaterial)) {
            reject('only StandardMaterial is supported by instanced skin.');
        }

        const mesh = meshInstance.mesh;
        const vertexBuffer = mesh.vertexBuffer;
        if (!vertexBuffer) {
            reject('the mesh has no vertex buffer.');
        }
        if (mesh.morph) {
            reject('a mesh with morph targets is not supported.');
        }
        if (mesh.primitive[0].type !== PRIMITIVE_TRIANGLES) {
            reject('only triangle meshes are supported.');
        }

        const format = vertexBuffer.format;
        if (!format.elements.find(e => e.name === SEMANTIC_POSITION)) {
            reject('the mesh has no position vertex stream.');
        }
        if (format.hasUv(6) || format.hasUv(7)) {
            reject('texture coordinate sets 6 and 7 occupy the default instancing attribute locations and are not supported.');
        }
        const indexElement = format.elements.find(e => e.name === SEMANTIC_BLENDINDICES);
        const weightElement = format.elements.find(e => e.name === SEMANTIC_BLENDWEIGHT);
        if (!indexElement || !weightElement) {
            reject('the mesh has no blend indices or blend weights vertex stream.');
        }
        if (indexElement.numComponents !== 4 || weightElement.numComponents !== 4) {
            reject('only four-weight skinning is supported.');
        }

        // a material sampling UV set 6 or 7 would also claim the instance attribute locations
        for (let i = 0; i < standardMaterialTextureParameters.length; i++) {
            const mapName = standardMaterialTextureParameters[i];
            if (material[mapName] && (material[`${mapName}Uv`] === 6 || material[`${mapName}Uv`] === 7)) {
                reject('material maps sampling UV sets 6 or 7 are not supported.');
            }
        }

        // an empty list is a valid, zero-instance state
        if (entries.length === 0) {
            return new InstancedSkin(mesh.device, mesh, [], 0);
        }

        const skin = mesh.skin;
        if (!skin) {
            reject('the mesh has no skin.');
        }
        if (!Array.isArray(skin.inverseBindPose) || skin.inverseBindPose.length === 0 ||
            !Array.isArray(skin.boneNames) || skin.boneNames.length !== skin.inverseBindPose.length) {
            reject('the skin has inconsistent bone names and inverse bind pose data.');
        }

        const ownerNode = meshInstance.node;
        if (!ownerNode) {
            reject('the mesh instance has no node.');
        }
        const ownerWorld = ownerNode.getWorldTransform();
        if (!isFiniteMatrix(ownerWorld)) {
            reject('the node of the mesh instance has a non-finite world matrix.');
        }
        const ownerDet = determinant3(ownerWorld);
        if (!Number.isFinite(ownerDet) || ownerDet === 0) {
            reject('the node of the mesh instance has a singular world matrix.');
        }
        _invOwner.copy(ownerWorld).invert();

        const copied = [];
        let boneCount = 0;
        for (let i = 0; i < entries.length; i++) {
            const entry = entries[i];
            if (!entry || typeof entry !== 'object') {
                reject(`entry ${i} is not an object with skinInstance and node.`);
            }
            const skinInstance = entry.skinInstance;
            const node = entry.node;

            if (!(skinInstance instanceof SkinInstance)) {
                reject(`entry ${i} does not supply a SkinInstance.`);
            }
            if (!node || typeof node.getWorldTransform !== 'function') {
                reject(`entry ${i} does not supply a node.`);
            }
            if (skinInstance.skin !== skin) {
                reject(`entry ${i} uses a skin which is not the skin of the mesh.`);
            }
            if (!Array.isArray(skinInstance.bones) || skinInstance.bones.length === 0) {
                reject(`entry ${i} has no bones bound to its skin instance.`);
            }
            if (skinInstance.bones.length !== skin.inverseBindPose.length) {
                reject(`entry ${i} has ${skinInstance.bones.length} bones but the skin has ${skin.inverseBindPose.length}.`);
            }
            for (let b = 0; b < skinInstance.bones.length; b++) {
                if (!skinInstance.bones[b] || typeof skinInstance.bones[b].getWorldTransform !== 'function') {
                    reject(`entry ${i} has no node bound to bone ${b} of its skin instance.`);
                }
            }
            for (let b = 0; b < skin.inverseBindPose.length; b++) {
                if (!isFiniteMatrix(skin.inverseBindPose[b])) {
                    reject(`entry ${i} has a non-finite inverse bind pose for bone ${b}.`);
                }
            }

            if (i === 0) {
                boneCount = skinInstance.bones.length;
            } else if (skinInstance.bones.length !== boneCount) {
                reject(`entry ${i} has ${skinInstance.bones.length} bones, while entry 0 has ${boneCount}.`);
            }

            const nodeWorld = node.getWorldTransform();
            if (!isFiniteMatrix(nodeWorld)) {
                reject(`entry ${i} has a non-finite world matrix.`);
            }
            const nodeDet = determinant3(nodeWorld);
            if (!Number.isFinite(nodeDet) || nodeDet === 0) {
                reject(`entry ${i} has a singular world matrix.`);
            }

            // the owner and the entry can carry a world reflection together, but the entry cannot
            // have a different chirality, which would flip the faces of this character alone
            _relative.mulAffine2(_invOwner, nodeWorld);
            if (determinant3(_relative) <= 0) {
                reject(`entry ${i} is mirrored relative to the node of the mesh instance.`);
            }

            copied.push({ skinInstance, node });
        }

        // validate the weights once, against the common bone count
        InstancedSkin._validateWeights(mesh, boneCount);

        return new InstancedSkin(mesh.device, mesh, copied, boneCount);
    }

    /**
     * Validates the blend indices and weights of the mesh: exactly four weights per vertex, finite
     * and non-negative, summing to one within the accepted epsilon, and a valid bone index for
     * every positive weight.
     *
     * @param {Mesh} mesh - The mesh.
     * @param {number} boneCount - The number of bones of the skin.
     * @private
     */
    static _validateWeights(mesh, boneCount) {
        const iterator = new VertexIterator(mesh.vertexBuffer);
        const weights = iterator.element[SEMANTIC_BLENDWEIGHT];
        const indices = iterator.element[SEMANTIC_BLENDINDICES];
        const numVertices = mesh.vertexBuffer.numVertices;

        for (let v = 0; v < numVertices; v++) {
            let sum = 0;
            for (let k = 0; k < 4; k++) {
                const weight = weights.array[weights.index + k];
                if (!Number.isFinite(weight) || weight < 0) {
                    reject(`vertex ${v} has an invalid blend weight of ${weight}.`);
                }
                sum += weight;

                if (weight > 0) {
                    const boneIndex = indices.array[indices.index + k];
                    if (!Number.isInteger(boneIndex) || boneIndex < 0 || boneIndex >= boneCount) {
                        reject(`vertex ${v} has a weight for invalid bone index ${boneIndex}, while there are ${boneCount} bones.`);
                    }
                }
            }

            if (Math.abs(sum - 1) > WEIGHT_SUM_EPSILON) {
                reject(`vertex ${v} has blend weights summing to ${sum}, which differs from 1 by more than ${WEIGHT_SUM_EPSILON}.`);
            }

            iterator.next();
        }
    }
}

export { InstancedSkin };
