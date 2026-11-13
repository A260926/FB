import BoundingBox from './math/BoundingBox';
import mat4 from './glmatrix/mat4';

var IDENTITY = mat4.create();

var WEIGHT_THRESHOLD = 1e-4;

/**
 * Multiply a column-major mat4 (starting at `base`) with a vec4 and add the
 * result scaled by `weight` to `out`.
 * Mirrors glmatrix vec4.transformMat4 exactly, including the homogeneous w.
 */
function addWeightedVec4(out, m, base, x, y, z, w, weight) {
    out[0] += weight * (m[base] * x + m[base + 4] * y + m[base + 8] * z + m[base + 12] * w);
    out[1] += weight * (m[base + 1] * x + m[base + 5] * y + m[base + 9] * z + m[base + 13] * w);
    out[2] += weight * (m[base + 2] * x + m[base + 6] * y + m[base + 10] * z + m[base + 14] * w);
    out[3] += weight * (m[base + 3] * x + m[base + 7] * y + m[base + 11] * z + m[base + 15] * w);
}

/**
 * Compute the current skinned world data of a mesh on the CPU.
 *
 * The deformation follows the shader rule in clay.chunk.skin_matrix: the first
 * weight/joint pair is always applied, the other three only when their weight
 * is strictly greater than 1e-4. The fourth weight is
 * `1 - weight.x - weight.y - weight.z`. Weighting is performed on the
 * homogeneous vec4 positions before any perspective divide, then the
 * offsetMatrix is applied and only afterwards divided by the homogeneous
 * denominator.
 *
 * Nothing is written to the shared geometry (position, boundingBox or a
 * skinnedPosition attribute), every call allocates its own result, so meshes
 * sharing one geometry never overwrite each other.
 *
 * @param {clay.Mesh} mesh
 * @return {{positions: Float32Array, boundingBox: clay.BoundingBox}}
 */
export function computeSkinnedWorld(mesh) {
    var geometry = mesh.geometry;
    var attributes = geometry.attributes;
    var positionAttr = attributes.position;
    var weightAttr = attributes.weight;
    var jointAttr = attributes.joint;

    var vertexCount = geometry.vertexCount;
    var positions = new Float32Array(vertexCount * 3);

    // Local joint palette, ordered like mesh.joints.
    var palette = mesh.skeleton.getSubSkinMatrices(mesh.__uid__, mesh.joints);
    var offsetArray = mesh.offsetMatrix ? mesh.offsetMatrix.array : IDENTITY;

    var pos = [];
    var weight = [];
    var joint = [];

    // Homogeneous skinned position before and after the offset.
    var skinned = [0, 0, 0, 0];

    var minX = Infinity, minY = Infinity, minZ = Infinity;
    var maxX = -Infinity, maxY = -Infinity, maxZ = -Infinity;

    for (var i = 0; i < vertexCount; i++) {
        positionAttr.get(i, pos);
        weightAttr.get(i, weight);
        jointAttr.get(i, joint);

        var x = pos[0], y = pos[1], z = pos[2];

        var w0 = weight[0];
        var w1 = weight[1];
        var w2 = weight[2];
        var w3 = 1 - w0 - w1 - w2;

        skinned[0] = 0;
        skinned[1] = 0;
        skinned[2] = 0;
        skinned[3] = 0;

        // The first term joins unconditionally, even with a zero weight.
        addWeightedVec4(skinned, palette, joint[0] * 16, x, y, z, 1, w0);
        // The remaining terms use the strict shader threshold, no renormalizing.
        if (w1 > WEIGHT_THRESHOLD) {
            addWeightedVec4(skinned, palette, joint[1] * 16, x, y, z, 1, w1);
        }
        if (w2 > WEIGHT_THRESHOLD) {
            addWeightedVec4(skinned, palette, joint[2] * 16, x, y, z, 1, w2);
        }
        if (w3 > WEIGHT_THRESHOLD) {
            addWeightedVec4(skinned, palette, joint[3] * 16, x, y, z, 1, w3);
        }

        // Offset of the instance, applied while still in homogeneous space.
        var ox = skinned[0], oy = skinned[1], oz = skinned[2], ow = skinned[3];
        var m = offsetArray;
        var hx = m[0] * ox + m[4] * oy + m[8] * oz + m[12] * ow;
        var hy = m[1] * ox + m[5] * oy + m[9] * oz + m[13] * ow;
        var hz = m[2] * ox + m[6] * oy + m[10] * oz + m[14] * ow;
        var hw = m[3] * ox + m[7] * oy + m[11] * oz + m[15] * ow;

        var invW = 1 / hw;
        var wx = hx * invW;
        var wy = hy * invW;
        var wz = hz * invW;

        var cursor = i * 3;
        positions[cursor] = wx;
        positions[cursor + 1] = wy;
        positions[cursor + 2] = wz;

        if (wx < minX) { minX = wx; }
        if (wy < minY) { minY = wy; }
        if (wz < minZ) { minZ = wz; }
        if (wx > maxX) { maxX = wx; }
        if (wy > maxY) { maxY = wy; }
        if (wz > maxZ) { maxZ = wz; }
    }

    var boundingBox = new BoundingBox();
    boundingBox.min.set(minX, minY, minZ);
    boundingBox.max.set(maxX, maxY, maxZ);

    return {
        positions: positions,
        boundingBox: boundingBox
    };
}

/**
 * Skinned world positions of every vertex, in vertex order.
 * @param {clay.Mesh} mesh
 * @return {Float32Array}
 */
export function computeSkinnedWorldPositions(mesh) {
    return computeSkinnedWorld(mesh).positions;
}

/**
 * Bounding box enclosing the skinned world positions of the mesh.
 * @param {clay.Mesh} mesh
 * @return {clay.BoundingBox}
 */
export function computeSkinnedWorldBoundingBox(mesh) {
    return computeSkinnedWorld(mesh).boundingBox;
}
