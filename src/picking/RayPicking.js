import Base from '../core/Base';
import Ray from '../math/Ray';
import Vector2 from '../math/Vector2';
import Vector3 from '../math/Vector3';
import Matrix4 from '../math/Matrix4';
import Renderable from '../Renderable';
import glenum from '../core/glenum';

/**
 * @constructor clay.picking.RayPicking
 * @extends clay.core.Base
 */
var RayPicking = Base.extend(/** @lends clay.picking.RayPicking# */{
    /**
     * Target scene
     * @type {clay.Scene}
     */
    scene: null,
    /**
     * Target camera
     * @type {clay.Camera}
     */
    camera: null,
    /**
     * Target renderer
     * @type {clay.Renderer}
     */
    renderer: null
}, function () {
    this._ray = new Ray();
    this._ndc = new Vector2();
},
/** @lends clay.picking.RayPicking.prototype */
{

    /**
     * Pick the nearest intersection object in the scene
     * @param  {number} x Mouse position x
     * @param  {number} y Mouse position y
     * @param  {boolean} [forcePickAll=false] ignore ignorePicking
     * @return {clay.picking.RayPicking~Intersection}
     */
    pick: function (x, y, forcePickAll) {
        var out = this.pickAll(x, y, [], forcePickAll);
        return out[0] || null;
    },

    /**
     * Pick all intersection objects, wich will be sorted from near to far
     * @param  {number} x Mouse position x
     * @param  {number} y Mouse position y
     * @param  {Array} [output]
     * @param  {boolean} [forcePickAll=false] ignore ignorePicking
     * @return {Array.<clay.picking.RayPicking~Intersection>}
     */
    pickAll: function (x, y, output, forcePickAll) {
        this.renderer.screenToNDC(x, y, this._ndc);
        this.camera.castRay(this._ndc, this._ray);

        output = output || [];

        this._intersectNode(this.scene, output, forcePickAll || false);

        output.sort(this._intersectionCompareFunc);

        return output;
    },

    _intersectNode: function (node, out, forcePickAll) {
        if ((node instanceof Renderable) && node.isRenderable()) {
            if ((!node.ignorePicking || forcePickAll)
                && (
                    // Only triangle mesh support ray picking
                    (node.mode === glenum.TRIANGLES && node.geometry.isUseIndices())
                    // Or if geometry has it's own pickByRay, pick, implementation
                    || node.geometry.pickByRay
                    || node.geometry.pick
                )
            ) {
                this._intersectRenderable(node, out);
            }
        }
        for (var i = 0; i < node._children.length; i++) {
            this._intersectNode(node._children[i], out, forcePickAll);
        }
    },

    _intersectRenderable: (function () {

        var v1 = new Vector3();
        var v2 = new Vector3();
        var v3 = new Vector3();
        var ray = new Ray();
        var worldInverse = new Matrix4();
        var offsetInverse = new Matrix4();

        return function (renderable, out) {

            var isSkinnedMesh = renderable.isSkinnedMesh();
            ray.copy(this._ray);
            Matrix4.invert(worldInverse, renderable.worldTransform);

            // Skinned mesh will ignore the world transform.
            if (!isSkinnedMesh) {
                ray.applyTransform(worldInverse);
            }

            var geometry = renderable.geometry;

            // Skinned mesh is deformed by its own skeleton pose and
            // offsetMatrix. Both the broad phase and the triangle
            // intersection use the world space positions of this very
            // instance, so the ray stays in world space.
            var skinnedWorldPositions = null;
            var bbox;
            if (isSkinnedMesh) {
                bbox = renderable.getSkinnedWorldBoundingBox();
            }
            else {
                bbox = geometry.boundingBox;
            }

            if (bbox && !ray.intersectBoundingBox(bbox)) {
                return;
            }
            // Use user defined picking algorithm
            if (geometry.pick) {
                geometry.pick(
                    this._ndc.x, this._ndc.y,
                    this.renderer,
                    this.camera,
                    renderable, out
                );
                return;
            }
            // Use user defined ray picking algorithm
            else if (geometry.pickByRay) {
                geometry.pickByRay(ray, renderable, out);
                return;
            }

            var cullBack = (renderable.cullFace === glenum.BACK && renderable.frontFace === glenum.CCW)
                        || (renderable.cullFace === glenum.FRONT && renderable.frontFace === glenum.CW);

            var point;
            var indices = geometry.indices;
            var positionAttr = geometry.attributes.position;
            // Check if valid.
            if (!positionAttr || !positionAttr.value || !indices) {
                return;
            }
            if (isSkinnedMesh) {
                skinnedWorldPositions = renderable.getSkinnedWorldPositions();
            }

            for (var i = 0; i < indices.length; i += 3) {
                var i1 = indices[i];
                var i2 = indices[i + 1];
                var i3 = indices[i + 2];
                if (isSkinnedMesh) {
                    var o1 = i1 * 3;
                    var o2 = i2 * 3;
                    var o3 = i3 * 3;
                    v1.array[0] = skinnedWorldPositions[o1];
                    v1.array[1] = skinnedWorldPositions[o1 + 1];
                    v1.array[2] = skinnedWorldPositions[o1 + 2];
                    v2.array[0] = skinnedWorldPositions[o2];
                    v2.array[1] = skinnedWorldPositions[o2 + 1];
                    v2.array[2] = skinnedWorldPositions[o2 + 2];
                    v3.array[0] = skinnedWorldPositions[o3];
                    v3.array[1] = skinnedWorldPositions[o3 + 1];
                    v3.array[2] = skinnedWorldPositions[o3 + 2];
                }
                else {
                    positionAttr.get(i1, v1.array);
                    positionAttr.get(i2, v2.array);
                    positionAttr.get(i3, v3.array);
                }

                if (cullBack) {
                    point = ray.intersectTriangle(v1, v2, v3, renderable.culling);
                }
                else {
                    point = ray.intersectTriangle(v1, v3, v2, renderable.culling);
                }
                if (point) {
                    var pointW = new Vector3();
                    if (!isSkinnedMesh) {
                        Vector3.transformMat4(pointW, point, renderable.worldTransform);
                    }
                    else {
                        // The intersection is already in world space.
                        Vector3.copy(pointW, point);
                        // The local point of a skinned mesh is the deformed
                        // intersection before offsetMatrix is applied.
                        if (renderable.offsetMatrix) {
                            Matrix4.invert(offsetInverse, renderable.offsetMatrix);
                            Vector3.transformMat4(point, point, offsetInverse);
                        }
                    }
                    out.push(new RayPicking.Intersection(
                        point, pointW, renderable, [i1, i2, i3], i / 3,
                        Vector3.dist(pointW, this._ray.origin)
                    ));
                }
            }
        };
    })(),

    _intersectionCompareFunc: function (a, b) {
        return a.distance - b.distance;
    }
});

/**
 * @constructor clay.picking.RayPicking~Intersection
 * @param {clay.Vector3} point
 * @param {clay.Vector3} pointWorld
 * @param {clay.Node} target
 * @param {Array.<number>} triangle
 * @param {number} triangleIndex
 * @param {number} distance
 */
RayPicking.Intersection = function (point, pointWorld, target, triangle, triangleIndex, distance) {
    /**
     * Intersection point in local transform coordinates
     * @type {clay.Vector3}
     */
    this.point = point;
    /**
     * Intersection point in world transform coordinates
     * @type {clay.Vector3}
     */
    this.pointWorld = pointWorld;
    /**
     * Intersection scene node
     * @type {clay.Node}
     */
    this.target = target;
    /**
     * Intersection triangle, which is an array of vertex index
     * @type {Array.<number>}
     */
    this.triangle = triangle;
    /**
     * Index of intersection triangle.
     */
    this.triangleIndex = triangleIndex;
    /**
     * Distance from intersection point to ray origin
     * @type {number}
     */
    this.distance = distance;
};

export default RayPicking;
