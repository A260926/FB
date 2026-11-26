import { Mat4 } from '../core/math/mat4.js';
import { GraphNode } from './graph-node.js';

// Scratch matrices reused across the fold math so the per-node cost stays allocation free.
const inverseAncestor = new Mat4();
const foldedLocal = new Mat4();

/**
 * Checks that a 4x4 matrix is a finite, invertible affine transform: the last column is
 * [0, 0, 0, 1], every element is finite and the 3x3 part has a non-zero determinant.
 *
 * @param {Mat4} matrix - The matrix to validate.
 * @returns {boolean} True if the matrix is a finite invertible affine matrix.
 * @private
 */
function isFiniteInvertibleAffine(matrix) {
    const m = matrix.data;

    if (m[3] !== 0 || m[7] !== 0 || m[11] !== 0 || m[15] !== 1) {
        return false;
    }

    for (let i = 0; i < 16; i++) {
        if (!Number.isFinite(m[i])) {
            return false;
        }
    }

    const det = m[0] * (m[5] * m[10] - m[6] * m[9]) -
                m[4] * (m[1] * m[10] - m[2] * m[9]) +
                m[8] * (m[1] * m[6] - m[2] * m[5]);

    return Number.isFinite(det) && det !== 0;
}

/**
 * Flattens a set of static intermediate pivot nodes out of a hierarchy. Each pivot's children are
 * lifted, in their original depth-first order, into the slot the pivot occupied under its parent,
 * while the pivot itself is detached as a parentless, childless node that is not destroyed. Every
 * retained node keeps its identity, local `enabled` state, tags and world transform, so the visible
 * scene does not change while the per-frame hierarchy has fewer levels to propagate.
 *
 * Only plain {@link GraphNode} instances below `root` are eligible: `root` itself and subclasses
 * (such as `Entity`) are never folded. Every pivot must be enabled, and the whole participating
 * subtree must not use scale compensation. Pivots may be nested, and the result is independent of
 * the order in which they appear in `pivots`. Duplicate pivots, pivots outside the `root` tree,
 * unsupported nodes and transforms that are not finite, invertible affine matrices are rejected
 * with a {@link RangeError} before anything in the hierarchy is modified.
 *
 * The moved nodes switch to explicit affine local matrices ({@link GraphNode#isLocalMatrix}),
 * which preserves the shear produced by non-uniformly scaled ancestors combined with rotations.
 * Frozen ancestors rejoin synchronization, and normal matrices, world scale signs and bounding box
 * versions are invalidated as part of the operation. The engine's regular `insert`,
 * `inserthierarchy`, `remove` and `removehierarchy` events fire, and an `insert` handler already
 * observes the folded hierarchy and its correct matrices.
 *
 * @param {GraphNode} root - The hierarchy root. It is never folded itself.
 * @param {Iterable<GraphNode>} pivots - The static, removable pivot nodes, confirmed by the caller.
 * @returns {GraphNode[]} The detached pivot nodes, in the order in which they were folded.
 * @example
 * // Collapse two static pivots under the assembly root
 * const detached = flattenStaticHierarchy(assemblyRoot, [pivotA, pivotB]);
 */
function flattenStaticHierarchy(root, pivots) {
    if (!(root instanceof GraphNode)) {
        throw new RangeError('flattenStaticHierarchy: root must be a GraphNode');
    }

    const pivotList = Array.from(pivots);
    const pivotSet = new Set();

    // ---- Phase 1: structural validation, before touching the hierarchy ----
    for (const pivot of pivotList) {
        if (!(pivot instanceof GraphNode) || pivot.constructor !== GraphNode) {
            throw new RangeError('flattenStaticHierarchy: every pivot must be a plain GraphNode');
        }
        if (pivot === root || !pivot.isDescendantOf(root)) {
            throw new RangeError('flattenStaticHierarchy: every pivot must be a descendant of root');
        }
        if (pivotSet.has(pivot)) {
            throw new RangeError('flattenStaticHierarchy: pivots must not contain duplicates');
        }
        if (!pivot._enabled) {
            throw new RangeError('flattenStaticHierarchy: every pivot must be enabled');
        }
        pivotSet.add(pivot);
    }

    // Scale compensation composes world transforms through TRS-only helpers that cannot express
    // the sheared affine matrices the fold may produce. Subclasses (such as Entity) are out of
    // scope: moving one into local matrix mode would break the TRS based APIs its own code uses.
    let supported = true;
    root.forEach((node) => {
        if (node.scaleCompensation) {
            supported = false;
        }
    });
    for (const pivot of pivotList) {
        pivot.forEach((node) => {
            if (node.constructor !== GraphNode) {
                supported = false;
            }
        });
    }
    if (!supported) {
        throw new RangeError('flattenStaticHierarchy: only plain GraphNode subtrees without scale compensation are supported');
    }

    // ---- Phase 2: bring every world matrix up to date, however dirty or frozen the ancestors are ----
    // World transforms are the invariant of the whole operation, so syncing once up front means
    // every folded local matrix can be derived from them regardless of pivot input order.
    root.forEach((node) => {
        node.getWorldTransform();
    });

    // ---- Phase 3: plan the folded tree and precompute/validate every folded local matrix ----
    // A depth-first walk treats pivots as transparent: each retained node is assigned its final
    // parent (nearest non-pivot ancestor). Moves remember the pivot directly beneath that parent
    // which contains the node, so execution can insert at the pivot's live slot while every pivot
    // is still attached.
    const finalLocals = new Map();
    const moves = [];

    const plan = (node, retainedParent) => {
        for (const child of node._children) {
            if (pivotSet.has(child)) {
                // Pivots disappear; their children take their slot under the same retained parent.
                plan(child, retainedParent);
            } else {
                if (child._parent !== retainedParent) {
                    // The child currently hangs below a pivot, so it will move. Its world matrix
                    // stays unchanged and its new local is retainedParentWorld^-1 * childWorld.
                    if (!isFiniteInvertibleAffine(retainedParent.worldTransform) ||
                        !isFiniteInvertibleAffine(child.worldTransform)) {
                        throw new RangeError('flattenStaticHierarchy: folded transforms must be finite invertible affine matrices');
                    }

                    inverseAncestor.copy(retainedParent.worldTransform).invert();
                    foldedLocal.mulAffine2(inverseAncestor, child.worldTransform);

                    if (!isFiniteInvertibleAffine(foldedLocal)) {
                        throw new RangeError('flattenStaticHierarchy: folded child transform must be a finite invertible affine matrix');
                    }

                    // Walk up the pivot chain to the pivot directly beneath the final parent; that
                    // pivot's live slot is where the child (and its pivot-siblings) must be inserted.
                    let anchor = child._parent;
                    while (anchor._parent !== retainedParent) {
                        anchor = anchor._parent;
                    }

                    finalLocals.set(child, foldedLocal.clone());
                    moves.push({ node: child, parent: retainedParent, anchor });
                }

                plan(child, child);
            }
        }
    };

    for (const pivot of pivotList) {
        if (!isFiniteInvertibleAffine(pivot.localTransform) ||
            !isFiniteInvertibleAffine(pivot.worldTransform)) {
            throw new RangeError('flattenStaticHierarchy: pivot transforms must be finite invertible affine matrices');
        }
    }

    plan(root, root);

    // ---- Phase 4: move retained nodes while the pivots are still attached ----
    // Depth-first planning order guarantees a retained parent is moved before its descendants.
    // Inserting at the anchor pivot's live index keeps the original relative child order even when
    // pivots are siblings of retained nodes or of other pivots. Every move goes through the
    // engine's own insert/remove machinery, so each retained node sees exactly one standard
    // reparent with in-tree parents, and the insert handler already reads the folded matrix.
    for (const move of moves) {
        move.node.setLocalMatrix(finalLocals.get(move.node));
        const index = move.parent._children.indexOf(move.anchor);
        move.parent.insertChild(move.node, index);
    }

    // The pivots are now childless; detach them deepest first without destroying them. The caller
    // still owns every pivot reference, and each one ends up both parentless and childless.
    const removalOrder = pivotList.slice().sort((a, b) => {
        if (a === b) {
            return 0;
        }
        if (b.isDescendantOf(a)) {
            return 1;
        }
        if (a.isDescendantOf(b)) {
            return -1;
        }
        return 0;
    });

    for (const pivot of removalOrder) {
        pivot.remove();
    }

    const detached = removalOrder;

    // ---- Phase 5: refresh caches so nothing keeps pre-fold derived values ----
    // _dirtifyWorld unfreezes the ancestor chain, marks the retained subtree dirty, resets the
    // normal matrix and world scale sign caches and bumps the bounding box version everywhere.
    root._dirtifyWorld();
    if (root._enabled) {
        root.syncHierarchy();
    }

    return detached;
}

export { flattenStaticHierarchy };
