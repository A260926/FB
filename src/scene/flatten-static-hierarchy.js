import { Mat4 } from '../core/math/mat4.js';
import { GraphNode } from './graph-node.js';

/**
 * @import { Entity } from '../framework/entity.js'
 */

const LOCAL_MATRIX_AFFINE_EPSILON = 1e-6;

const tmpLocal = new Mat4();

// A local transform can be folded into descendants only if it is finite, affine and invertible.
function isValidLocalMatrix(matrix) {
    const d = matrix.data;

    for (let i = 0; i < 16; i++) {
        if (!Number.isFinite(d[i])) {
            return false;
        }
    }

    if (Math.abs(d[3]) > LOCAL_MATRIX_AFFINE_EPSILON ||
        Math.abs(d[7]) > LOCAL_MATRIX_AFFINE_EPSILON ||
        Math.abs(d[11]) > LOCAL_MATRIX_AFFINE_EPSILON ||
        Math.abs(d[15] - 1) > LOCAL_MATRIX_AFFINE_EPSILON) {
        return false;
    }

    const det = d[0] * (d[5] * d[10] - d[6] * d[9]) -
                d[4] * (d[1] * d[10] - d[2] * d[9]) +
                d[8] * (d[1] * d[6] - d[2] * d[5]);

    return Number.isFinite(det) && det !== 0;
}

// Walk the local enabled flags rather than cached `_enabledInHierarchy`, which is only refreshed
// when nodes are inserted or an enabled flag changes.
function isEnabledThrough(node, root) {
    let current = node;
    while (current) {
        if (!current._enabled) {
            return false;
        }
        if (current === root) {
            break;
        }
        current = current.parent;
    }
    return true;
}

// A scale-compensated node computes its world transform in a special way rather than as
// `parentWorld * local`, so a pivot subtree using it cannot be folded with a plain matrix product.
// The same walk rejects any node whose local matrix cannot be used for folding.
function validateSubtree(pivot) {
    pivot.forEach((node) => {
        if (node.scaleCompensation) {
            throw new RangeError(`flattenStaticHierarchy pivot '${pivot.name}' has a subtree using scaleCompensation.`);
        }
        if (!isValidLocalMatrix(node.getLocalTransform())) {
            throw new RangeError(`flattenStaticHierarchy pivot '${pivot.name}' has a node with an invalid local matrix.`);
        }
    });
}

/**
 * Flattens a set of static pivot nodes out of a hierarchy.
 *
 * Each pivot's children are promoted to the pivot's parent, taking the pivot's position in the
 * parent's child list and keeping their original relative order. The pivot itself is left as a
 * parentless, childless node which is not destroyed. This removes a level of hierarchy from
 * static content, so that per-frame hierarchy sync has fewer nodes to visit.
 *
 * The promotion preserves every retained node's world transform. Because a pivot under a
 * non-uniformly scaled parent can produce a local matrix containing shear, promoted nodes are
 * placed into {@link GraphNode#isLocalMatrix local matrix mode} with
 * {@link GraphNode#setLocalMatrix}, so the transform is never decomposed back into a lossy TRS.
 * Pivots may be nested and the result does not depend on the order of the `pivots` array.
 *
 * Only plain {@link GraphNode} instances that are strict descendants of `root` can be folded;
 * `root` itself and subclasses such as {@link Entity} are rejected as pivots. Every pivot must be
 * enabled, and neither a pivot nor any of its descendants may use `scaleCompensation`. All
 * validation is performed before the hierarchy is modified.
 *
 * @param {GraphNode} root - The root of the hierarchy. Pivots must be strictly inside it.
 * @param {GraphNode[]} pivots - The static pivots to remove from the hierarchy.
 * @returns {GraphNode[]} The folded pivots, in hierarchy order.
 * @throws {TypeError} If `root` is not a GraphNode, `pivots` is not an array, or an entry is not a
 * plain GraphNode.
 * @throws {RangeError} If a pivot is `root`, is not inside `root`, is listed more than once, is
 * not enabled, participates in `scaleCompensation`, or has a local matrix which is not finite,
 * affine and invertible.
 * @example
 * // Collapse static assembly pivots after loading
 * const pivots = assembly.find((node) => {
 *     return node.tags.has('static-pivot');
 * });
 * flattenStaticHierarchy(assembly.root, pivots);
 */
function flattenStaticHierarchy(root, pivots) {
    if (!(root instanceof GraphNode)) {
        throw new TypeError('flattenStaticHierarchy expects a GraphNode as the root.');
    }
    if (!Array.isArray(pivots)) {
        throw new TypeError('flattenStaticHierarchy expects an array of pivots.');
    }

    // Validate the pivots themselves first.
    const seen = new Set();
    for (let i = 0; i < pivots.length; i++) {
        const pivot = pivots[i];

        if (!(pivot instanceof GraphNode) || pivot.constructor !== GraphNode) {
            throw new TypeError('flattenStaticHierarchy only supports plain GraphNode pivots.');
        }
        if (seen.has(pivot)) {
            throw new RangeError(`flattenStaticHierarchy received duplicate pivot '${pivot.name}'.`);
        }
        seen.add(pivot);

        if (pivot === root) {
            throw new RangeError('flattenStaticHierarchy cannot fold the root node.');
        }
        if (!pivot.isDescendantOf(root)) {
            throw new RangeError(`flattenStaticHierarchy pivot '${pivot.name}' is not inside the root.`);
        }
        if (!pivot._enabled) {
            throw new RangeError(`flattenStaticHierarchy pivot '${pivot.name}' is not enabled.`);
        }
        if (!isEnabledThrough(pivot, root)) {
            throw new RangeError(`flattenStaticHierarchy pivot '${pivot.name}' is disabled through its ancestors.`);
        }
    }

    // Validate the participating subtrees before touching the hierarchy, so that an unsupported
    // hierarchy is rejected as a whole rather than half-folded.
    for (let i = 0; i < pivots.length; i++) {
        validateSubtree(pivots[i]);
    }

    if (pivots.length === 0) {
        return [];
    }

    // Resolve the processing order from the hierarchy itself, not from the input array, so that
    // the result is independent of the pivot order. Pre-order visits ancestors before
    // descendants, which keeps nested pivots valid as they are folded.
    const order = new Map();
    let index = 0;
    root.forEach((/** @type {GraphNode} */ node) => {
        order.set(node, index++);
    });

    const sorted = pivots.slice().sort((a, b) => order.get(a) - order.get(b));

    for (let i = 0; i < sorted.length; i++) {
        const pivot = sorted[i];
        const parent = pivot.parent;

        // A pivot is a strict descendant of root, so it still has a parent here: folding an
        // ancestor promotes its children instead of destroying them.
        if (!parent) {
            continue;
        }

        const pivotIndex = parent.children.indexOf(pivot);
        const children = pivot.children.slice();
        const pivotLocal = pivot.getLocalTransform();

        for (let j = 0; j < children.length; j++) {
            const child = children[j];

            // parentWorld * (pivotLocal * childLocal) equals parentWorld * pivotLocal * childLocal,
            // so the child keeps its world transform. The product is installed directly as a
            // local matrix so that shear introduced by the pivot is preserved.
            tmpLocal.mulAffine2(pivotLocal, child.getLocalTransform());
            child.setLocalMatrix(tmpLocal);

            parent.insertChild(child, pivotIndex + j);
        }

        // The pivot is left as a detached, empty node; it is not destroyed.
        pivot.remove();
        pivot._dirtifyWorld();
    }

    return sorted;
}

export { flattenStaticHierarchy };
