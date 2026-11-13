import Base from '../core/Base';
import Ray from '../math/Ray';
import Vector2 from '../math/Vector2';
import Vector3 from '../math/Vector3';
import Matrix4 from '../math/Matrix4';
import Renderable from '../Renderable';
import glenum from '../core/glenum';
import vec3 from '../glmatrix/vec3';
import {computeSkinnedWorld} from '../skinning';

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

            var hasCustomPick = !!geometry.pick;
            var hasCustomRayPick = !!geometry.pickByRay;

            // For a skinned mesh on the default triangle path the wide phase and
            // the triangle tests share the same per-instance deformed bounds and
            // world points. The shared geometry is never written to. Meshes with
            // user-defined picking keep the original skeleton-box wide phase and
            // skip the CPU skinning altogether.
            var skinnedData = null;
            var bbox;
            if (isSkinnedMesh) {
                if (hasCustomPick || hasCustomRayPick) {
                    bbox = renderable.skeleton.boundingBox;
                }
                else {
                    skinnedData = computeSkinnedWorld(renderable);
                    bbox = skinnedData.boundingBox;
                }
            }
            else {
                bbox = geometry.boundingBox;
            }

            if (bbox && !ray.intersectBoundingBox(bbox)) {
                return;
            }
            // Use user defined picking algorithm
            if (hasCustomPick) {
                geometry.pick(
                    this._ndc.x, this._ndc.y,
                    this.renderer,
                    this.camera,
                    renderable, out
                );
                return;
            }
            // Use user defined ray picking algorithm
            else if (hasCustomRayPick) {
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

            var skinnedPositions = isSkinnedMesh ? skinnedData.positions : null;

            for (var i = 0; i < indices.length; i += 3) {
                var i1 = indices[i];
                var i2 = indices[i + 1];
                var i3 = indices[i + 2];

                if (isSkinnedMesh) {
                    var b1 = i1 * 3;
                    var b2 = i2 * 3;
                    var b3 = i3 * 3;
                    vec3.set(v1.array, skinnedPositions[b1], skinnedPositions[b1 + 1], skinnedPositions[b1 + 2]);
                    vec3.set(v2.array, skinnedPositions[b2], skinnedPositions[b2 + 1], skinnedPositions[b2 + 2]);
                    vec3.set(v3.array, skinnedPositions[b3], skinnedPositions[b3 + 1], skinnedPositions[b3 + 2]);
                    v1._dirty = v2._dirty = v3._dirty = true;
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
                        // The triangle is already in skinned world space.
                        Vector3.copy(pointW, point);
                        // The reported point is the deformed intersection before
                        // the instance offset, pointWorld keeps the offset applied.
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
