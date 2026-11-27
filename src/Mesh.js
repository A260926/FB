import Renderable from './Renderable';
import glenum from './core/glenum';
import BoundingBox from './math/BoundingBox';

/**
 * @constructor clay.Mesh
 * @extends clay.Renderable
 */
var Mesh = Renderable.extend(/** @lends clay.Mesh# */ {
    /**
     * Used when it is a skinned mesh
     * @type {clay.Skeleton}
     */
    skeleton: null,
    /**
     * Joints indices Meshes can share the one skeleton instance and each mesh can use one part of joints. Joints indices indicate the index of joint in the skeleton instance
     * @type {number[]}
     */
    joints: null

}, function () {
    if (!this.joints) {
        this.joints = [];
    }
}, {

    /**
     * Offset matrix used for multiple skinned mesh clone sharing one skeleton
     * @type {clay.Matrix4}
     */
    offsetMatrix: null,

    isInstancedMesh: function () { return false; },

    isSkinnedMesh: function () {
        return !!(this.skeleton && this.joints && this.joints.length > 0);
    },

    /**
     * Get the world space vertex positions of this skinned mesh, deformed by
     * the current skeleton pose, the local joint palette and offsetMatrix.
     *
     * The deformation follows the same rule as the skinning chunk of the
     * builtin shaders (clay.chunk.skin_matrix): joint matrices are
     * `jointWorldTransform * inverseBindMatrix` of the joints referenced by
     * {@link clay.Mesh#joints} (see {@link clay.Skeleton#getSubSkinMatrices}).
     * The first weight is always added, the other three only when they are
     * larger than 1e-4. The fourth weight is `1 - weight.x - weight.y - weight.z`.
     * Homogeneous positions are weighted with this rule first, then
     * transformed by offsetMatrix (identity if not set) and finally divided
     * by the homogeneous denominator.
     *
     * The result is already in world space, worldTransform of the mesh
     * itself must not be applied anymore. Geometry attributes (position,
     * boundingBox, skinnedPosition) are never written and every call
     * returns a freshly computed array, so querying one instance never
     * overwrites the result another instance holds. The query does not
     * advance any animation, it reads the skeleton pose as it is.
     *
     * @method
     * @return {Float32Array} World space positions, 3 components per vertex, in vertex order.
     */
    getSkinnedWorldPositions: (function () {
        var pos = [];
        var weight = [];
        var joint = [];
        return function () {
            var geometry = this.geometry;
            var positionAttr = geometry.attributes.position;
            var weightAttr = geometry.attributes.weight;
            var jointAttr = geometry.attributes.joint;
            var vertexCount = geometry.vertexCount;

            // Local joint palette of this very mesh, maps the joint
            // attribute (local indices) to skin matrices of the skeleton.
            var skinMatrices = this.skeleton.getSubSkinMatrices(this.__uid__, this.joints);
            var offset = this.offsetMatrix ? this.offsetMatrix.array : null;

            var out = new Float32Array(vertexCount * 3);
            for (var i = 0; i < vertexCount; i++) {
                positionAttr.get(i, pos);
                weightAttr.get(i, weight);
                jointAttr.get(i, joint);

                var px = pos[0];
                var py = pos[1];
                var pz = pos[2];
                // Weight the homogeneous positions, normalize at the very
                // end instead of before filtering the weights.
                var hx = 0;
                var hy = 0;
                var hz = 0;
                var hw = 0;
                for (var k = 0; k < 4; k++) {
                    var w = k < 3 ? weight[k] : 1 - weight[0] - weight[1] - weight[2];
                    // Same threshold rule as clay.chunk.skin_matrix, the
                    // first weight is added unconditionally.
                    if (k > 0 && w <= 1e-4) {
                        continue;
                    }
                    var mo = joint[k] * 16;
                    hx += w * (skinMatrices[mo] * px + skinMatrices[mo + 4] * py + skinMatrices[mo + 8] * pz + skinMatrices[mo + 12]);
                    hy += w * (skinMatrices[mo + 1] * px + skinMatrices[mo + 5] * py + skinMatrices[mo + 9] * pz + skinMatrices[mo + 13]);
                    hz += w * (skinMatrices[mo + 2] * px + skinMatrices[mo + 6] * py + skinMatrices[mo + 10] * pz + skinMatrices[mo + 14]);
                    hw += w * (skinMatrices[mo + 3] * px + skinMatrices[mo + 7] * py + skinMatrices[mo + 11] * pz + skinMatrices[mo + 15]);
                }
                if (offset) {
                    var ox = offset[0] * hx + offset[4] * hy + offset[8] * hz + offset[12] * hw;
                    var oy = offset[1] * hx + offset[5] * hy + offset[9] * hz + offset[13] * hw;
                    var oz = offset[2] * hx + offset[6] * hy + offset[10] * hz + offset[14] * hw;
                    var ow = offset[3] * hx + offset[7] * hy + offset[11] * hz + offset[15] * hw;
                    hx = ox;
                    hy = oy;
                    hz = oz;
                    hw = ow;
                }
                var invW = 1 / hw;
                var idx = i * 3;
                out[idx] = hx * invW;
                out[idx + 1] = hy * invW;
                out[idx + 2] = hz * invW;
            }
            return out;
        };
    })(),

    /**
     * Get the bounding box enclosing the positions returned by
     * {@link clay.Mesh#getSkinnedWorldPositions}, in world space.
     * A new bounding box is computed on every call.
     * @return {clay.BoundingBox}
     */
    getSkinnedWorldBoundingBox: function () {
        var positions = this.getSkinnedWorldPositions();
        var boundingBox = new BoundingBox();
        var min = boundingBox.min.array;
        var max = boundingBox.max.array;
        for (var i = 0; i < positions.length; i += 3) {
            var x = positions[i];
            var y = positions[i + 1];
            var z = positions[i + 2];
            if (x < min[0]) { min[0] = x; }
            if (y < min[1]) { min[1] = y; }
            if (z < min[2]) { min[2] = z; }
            if (x > max[0]) { max[0] = x; }
            if (y > max[1]) { max[1] = y; }
            if (z > max[2]) { max[2] = z; }
        }
        boundingBox.min._dirty = true;
        boundingBox.max._dirty = true;
        return boundingBox;
    },

    clone: function () {
        var mesh = Renderable.prototype.clone.call(this);
        mesh.skeleton = this.skeleton;
        if (this.joints) {
            mesh.joints = this.joints.slice();
        }
        return mesh;
    }
});

// Enums
Mesh.POINTS = glenum.POINTS;
Mesh.LINES = glenum.LINES;
Mesh.LINE_LOOP = glenum.LINE_LOOP;
Mesh.LINE_STRIP = glenum.LINE_STRIP;
Mesh.TRIANGLES = glenum.TRIANGLES;
Mesh.TRIANGLE_STRIP = glenum.TRIANGLE_STRIP;
Mesh.TRIANGLE_FAN = glenum.TRIANGLE_FAN;

Mesh.BACK = glenum.BACK;
Mesh.FRONT = glenum.FRONT;
Mesh.FRONT_AND_BACK = glenum.FRONT_AND_BACK;
Mesh.CW = glenum.CW;
Mesh.CCW = glenum.CCW;

export default Mesh;
