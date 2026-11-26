import { Mat4 } from '../../src/core/math/mat4.js';
import { Quat } from '../../src/core/math/quat.js';
import { Vec3 } from '../../src/core/math/vec3.js';
import { BoundingBox } from '../../src/core/shape/bounding-box.js';
import { flattenStaticHierarchy } from '../../src/scene/flatten-static-hierarchy.js';
import { GraphNode } from '../../src/scene/graph-node.js';

const EPS = 1e-5;

// ---------------------------------------------------------------------------
// Independent reference math: plain array implementations that do not share
// any code path with the engine's Mat4/Mat3, so agreement is a real cross-check.
// ---------------------------------------------------------------------------

/**
 * Independent full 4x4 multiply on column-major arrays.
 *
 * @param {ArrayLike<number>} a - Left matrix.
 * @param {ArrayLike<number>} b - Right matrix.
 * @returns {Float32Array} a * b in column-major order.
 */
function mul4Ref(a, b) {
    const A = a.data ?? a;
    const B = b.data ?? b;
    const R = new Float32Array(16);
    for (let col = 0; col < 4; col++) {
        for (let row = 0; row < 4; row++) {
            let sum = 0;
            for (let k = 0; k < 4; k++) {
                sum += A[k * 4 + row] * B[col * 4 + k];
            }
            R[col * 4 + row] = sum;
        }
    }
    return R;
}

/**
 * Independent point transform by a column-major 4x4.
 *
 * @param {ArrayLike<number>} m - Matrix.
 * @param {Vec3} p - Point.
 * @returns {Vec3} Transformed point.
 */
function transformPointRef(m, p) {
    const M = m.data ?? m;
    return new Vec3(
        M[0] * p.x + M[4] * p.y + M[8] * p.z + M[12],
        M[1] * p.x + M[5] * p.y + M[9] * p.z + M[13],
        M[2] * p.x + M[6] * p.y + M[10] * p.z + M[14]
    );
}

/**
 * Independent reference for the engine's normal matrix, computed as the inverse-transpose
 * (A^-1)^T of the 3x3 linear part and packed as GPU mat3 columns, which is how
 * `Mat3.invertMat4(world).transpose()` stores its data. The inversion itself is a plain
 * Gauss-Jordan elimination on row-major arrays with no engine matrix code.
 *
 * @param {ArrayLike<number>} m - Column-major 4x4 world transform.
 * @returns {Float32Array} Normal matrix data, laid out as mat3 columns.
 */
function normalMatrixRef(m) {
    const M4 = m.data ?? m;

    // Extract the column-major 3x3 linear part from the column-major 4x4 (its columns start at
    // offsets 0, 4 and 8), then take a plain row-major view for the Gauss-Jordan inversion.
    const M = [
        M4[0], M4[1], M4[2],
        M4[4], M4[5], M4[6],
        M4[8], M4[9], M4[10]
    ];

    const aug = [
        [M[0], M[3], M[6], 1, 0, 0],
        [M[1], M[4], M[7], 0, 1, 0],
        [M[2], M[5], M[8], 0, 0, 1]
    ];

    for (let col = 0; col < 3; col++) {
        let pivot = col;
        for (let r = col + 1; r < 3; r++) {
            if (Math.abs(aug[r][col]) > Math.abs(aug[pivot][col])) {
                pivot = r;
            }
        }
        const tmp = aug[col];
        aug[col] = aug[pivot];
        aug[pivot] = tmp;

        const divisor = aug[col][col];
        for (let k = 0; k < 6; k++) {
            aug[col][k] /= divisor;
        }
        for (let r = 0; r < 3; r++) {
            if (r === col) {
                continue;
            }
            const factor = aug[r][col];
            for (let k = 0; k < 6; k++) {
                aug[r][k] -= factor * aug[col][k];
            }
        }
    }

    const inv = aug.map((row) => row.slice(3)); // row-major A^-1

    // The normal transform is (A^-1)^T; stored as GPU mat3 columns its element (row, col) is
    // inv[col][row], so column c of the storage is row c of A^-1: data[c*3+row] = inv[c][row].
    const out = new Float32Array(9);
    for (let col = 0; col < 3; col++) {
        for (let row = 0; row < 3; row++) {
            out[col * 3 + row] = inv[col][row];
        }
    }
    return out;
}

/**
 * Independent scale sign of a column-major 4x4: sign of the determinant of its
 * 3x3 linear part, evaluated through the scalar triple product of its columns.
 *
 * @param {ArrayLike<number>} m - Column-major 4x4.
 * @returns {number} -1 for a reflected transform, 1 otherwise.
 */
function scaleSignRef(m) {
    const M = m.data ?? m;
    const cx = [M[0], M[1], M[2]];
    const cy = [M[4], M[5], M[6]];
    const cz = [M[8], M[9], M[10]];
    const cross = [
        cx[1] * cy[2] - cx[2] * cy[1],
        cx[2] * cy[0] - cx[0] * cy[2],
        cx[0] * cy[1] - cx[1] * cy[0]
    ];
    const dot = cross[0] * cz[0] + cross[1] * cz[1] + cross[2] * cz[2];
    return dot < 0 ? -1 : 1;
}

function maxDiff(a, b) {
    const A = a.data ?? a;
    const B = b.data ?? b;
    let diff = 0;
    for (let i = 0; i < A.length; i++) {
        diff = Math.max(diff, Math.abs(A[i] - B[i]));
    }
    return diff;
}

function expectRangeError(check, fn, label) {
    let threw = false;
    try {
        fn();
    } catch (err) {
        threw = err instanceof RangeError;
    }
    check(threw, `${label} must throw RangeError`);
}

/**
 * Builds the shared stress scenario: non-coaxial rotations, non-uniform and
 * negative scales, two nested plain-GraphNode pivots and several retained nodes.
 *
 * @returns {object} The named nodes of the hierarchy.
 */
function buildScenario() {
    const root = new GraphNode('root');
    const a = new GraphNode('a');
    const p1 = new GraphNode('p1');
    const p2 = new GraphNode('p2');
    const leaf = new GraphNode('leaf');
    const sib = new GraphNode('sib');
    const keep = new GraphNode('keep');

    root.addChild(a);
    root.addChild(keep);
    a.addChild(p1);
    p1.addChild(p2);
    p1.addChild(sib);
    p2.addChild(leaf);

    root.setLocalPosition(1, 2, 3);
    root.setLocalEulerAngles(12, 0, 0);
    root.setLocalScale(2, -3, 1.5);

    a.setLocalPosition(4, -1, 2);
    a.setLocalEulerAngles(0, 35, 0);
    a.setLocalScale(1.5, 0.7, -2);

    p1.setLocalPosition(2, 0, 1);
    p1.setLocalEulerAngles(20, 0, 40);
    p1.setLocalScale(0.5, 2, 1.2);

    p2.setLocalPosition(-1, 1, 2);
    p2.setLocalEulerAngles(0, 55, 25);
    p2.setLocalScale(3, 0.4, -1);

    leaf.setLocalPosition(1, 1, 1);
    leaf.setLocalEulerAngles(30, 40, 50);
    leaf.setLocalScale(2, 2, 2);

    sib.setLocalPosition(5, -2, 0);
    sib.setLocalEulerAngles(0, 0, 30);

    keep.setLocalPosition(7, 8, 9);

    leaf.tags.add('leaf');
    sib.tags.add('sib');
    keep.tags.add('keep');

    return { root, a, p1, p2, leaf, sib, keep };
}

/**
 * Independently computes the world matrix chain of the original scenario using
 * local TRS matrices and plain array multiplication.
 *
 * @param {object} nodes - The scenario nodes.
 * @returns {Map<GraphNode, Float32Array>} Reference world matrices.
 */
function referenceWorlds(nodes) {
    const { root, a, p1, p2, leaf, sib, keep } = nodes;

    const wRoot = mul4Ref(new Mat4(), root.getLocalTransform());
    const wA = mul4Ref(wRoot, a.getLocalTransform());
    const wP1 = mul4Ref(wA, p1.getLocalTransform());
    const wP2 = mul4Ref(wP1, p2.getLocalTransform());
    const wLeaf = mul4Ref(wP2, leaf.getLocalTransform());
    const wSib = mul4Ref(wP1, sib.getLocalTransform());
    const wKeep = mul4Ref(wRoot, keep.getLocalTransform());

    return new Map([
        [root, wRoot],
        [a, wA],
        [p1, wP1],
        [p2, wP2],
        [leaf, wLeaf],
        [sib, wSib],
        [keep, wKeep]
    ]);
}

const cases = [
    {
        name: 'flatten preserves retained worlds with shear, nested pivots and unsynced ancestors',
        run(check) {
            const scenario = buildScenario();
            const { root, a, p1, p2, leaf, sib, keep } = scenario;

            // Sync once, then leave a dirty ancestor under a manually frozen root so the fold has
            // to walk and update an unsynchronized chain rather than trust cached worlds.
            root.syncHierarchy();
            a.setLocalPosition(40, -10, 20);
            root._frozen = true;
            const expected = referenceWorlds(scenario);

            const detached = flattenStaticHierarchy(root, [p1, p2]);

            check(detached.length === 2, 'both pivots are returned as detached');
            check(detached[0] === p1 || detached[0] === p2, 'detached list holds the pivot identities');
            check(p1.parent === null && p2.parent === null, 'pivots end up parentless');
            check(p1.children.length === 0 && p2.children.length === 0, 'pivots end up childless');
            check(p1.name === 'p1' && p2.name === 'p2', 'detached pivots are still live nodes');

            // Final structure: root -> [a, keep], a -> [leaf, sib]
            check(root.children.map((n) => n.name).join(',') === 'a,keep', 'root child order preserved');
            check(a.children.map((n) => n.name).join(',') === 'leaf,sib', 'pivot children lifted in original order');

            // Identities, local enabled and tags survive.
            check(leaf.tags.has('leaf') && sib.tags.has('sib') && keep.tags.has('keep'), 'tags preserved');
            check(leaf._enabled && sib._enabled && keep._enabled, 'local enabled preserved');

            // World matrices unchanged, cross-checked against independent multiplication.
            for (const node of [root, a, leaf, sib, keep]) {
                check(maxDiff(node.getWorldTransform(), expected.get(node)) < EPS,
                    `${node.name} world matrix preserved within 1e-5`);
            }

            // Folded locals genuinely carry shear (non-orthogonal axes), proving matrix mode is used.
            const local = leaf.getLocalTransform().data;
            const c0 = new Vec3(local[0], local[1], local[2]);
            const c1 = new Vec3(local[4], local[5], local[6]);
            const c2 = new Vec3(local[8], local[9], local[10]);
            check(Math.abs(c0.dot(c1)) > 1e-4 || Math.abs(c1.dot(c2)) > 1e-4 ||
                  Math.abs(c0.dot(c2)) > 1e-4, 'folded local matrix contains shear');
            check(leaf.isLocalMatrix && sib.isLocalMatrix, 'moved nodes are in local matrix mode');

            // Independent multiplication of the folded chain reproduces each expected world.
            const wAFolded = mul4Ref(root.getWorldTransform(), a.getLocalTransform());
            const wLeafFolded = mul4Ref(wAFolded, leaf.getLocalTransform());
            check(maxDiff(wLeafFolded, expected.get(leaf)) < EPS,
                'independent multiply of folded chain matches leaf world');

            // Negative scale reflections survive the fold, matched against the reference worlds.
            check(leaf.worldScaleSign === scaleSignRef(expected.get(leaf)),
                'negative scale sign preserved through fold');
            check(keep.worldScaleSign === scaleSignRef(expected.get(keep)),
                'positive scale sign preserved through fold');

            // AABB versions moved on retained descendants.
            check(leaf._aabbVer > 0 && sib._aabbVer > 0, 'bounding box versions invalidated');

            // Order independence: same inputs in reverse must produce the same result.
            const other = buildScenario();
            const otherExpected = referenceWorlds(other);
            flattenStaticHierarchy(other.root, [other.p2, other.p1]);
            check(other.root.children.map((n) => n.name).join(',') === 'a,keep',
                'order-independent root structure');
            check(other.a.children.map((n) => n.name).join(',') === 'leaf,sib',
                'order-independent lifted order');
            for (const [name, node] of [['leaf', other.leaf], ['sib', other.sib]]) {
                check(maxDiff(node.getWorldTransform(), otherExpected.get(
                    name === 'leaf' ? other.leaf : other.sib
                )) < EPS, `order-independent ${name} world matrix`);
            }

            // Sibling pivots interleaved with retained nodes must keep the original depth-first
            // order regardless of the order in which pivots are supplied.
            for (const reversed of [false, true]) {
                const r = new GraphNode();
                const pa = new GraphNode('pa');
                const pb = new GraphNode('pb');
                const xx = new GraphNode('x');
                const cc1 = new GraphNode('c1');
                const cc2 = new GraphNode('c2');
                const dd1 = new GraphNode('d1');
                r.addChild(pa);
                r.addChild(xx);
                r.addChild(pb);
                pa.addChild(cc1);
                pa.addChild(cc2);
                pb.addChild(dd1);
                pa.setLocalEulerAngles(0, 0, 30);
                xx.setLocalPosition(3, 2, 1);
                pb.setLocalScale(2, -1, 1);
                const worldsBefore = [cc1, cc2, dd1].map((n) => {
                    r.getWorldTransform();
                    return n.getWorldTransform().clone();
                });
                flattenStaticHierarchy(r, reversed ? [pb, pa] : [pa, pb]);
                check(r.children.map((n) => n.name).join(',') === 'c1,c2,x,d1',
                    `sibling pivot order preserved (${reversed ? 'reversed' : 'forward'} input)`);
                [cc1, cc2, dd1].forEach((n, i) => {
                    check(maxDiff(n.getWorldTransform(), worldsBefore[i]) < EPS,
                        `${n.name} world preserved with sibling pivots`);
                });
            }
        }
    },
    {
        name: 'world bounding box from eight corners and inverse-transpose normals',
        run(check) {
            const scenario = buildScenario();
            const { root, leaf } = scenario;
            const expected = referenceWorlds(scenario);

            flattenStaticHierarchy(root, [scenario.p1, scenario.p2]);

            // Eight box corners transformed independently define the world AABB.
            const localBox = new BoundingBox(new Vec3(0.3, -0.7, 0.2), new Vec3(1.4, 0.9, 1.1));
            const world = leaf.getWorldTransform();
            const corners = [];
            for (const sx of [-1, 1]) {
                for (const sy of [-1, 1]) {
                    for (const sz of [-1, 1]) {
                        corners.push(new Vec3(
                            localBox.center.x + sx * localBox.halfExtents.x,
                            localBox.center.y + sy * localBox.halfExtents.y,
                            localBox.center.z + sz * localBox.halfExtents.z
                        ));
                    }
                }
            }
            const transformed = corners.map((c) => transformPointRef(world, c));
            const min = new Vec3(Infinity, Infinity, Infinity);
            const max = new Vec3(-Infinity, -Infinity, -Infinity);
            for (const p of transformed) {
                min.set(Math.min(min.x, p.x), Math.min(min.y, p.y), Math.min(min.z, p.z));
                max.set(Math.max(max.x, p.x), Math.max(max.y, p.y), Math.max(max.z, p.z));
            }
            const cornerBox = new BoundingBox();
            cornerBox.setMinMax(min, max);

            const engineBox = new BoundingBox();
            engineBox.setFromTransformedAabb(localBox, world);

            check(maxDiff(cornerBox.center, engineBox.center) < EPS,
                'AABB center from eight corners matches engine');
            check(maxDiff(cornerBox.halfExtents, engineBox.halfExtents) < EPS,
                'AABB half extents from eight corners match engine');

            // Normal matrix independently recomputed as inverse transpose of the 3x3.
            const normalRef = normalMatrixRef(world);
            check(maxDiff(leaf.normalMatrix, normalRef) < EPS,
                'normal matrix matches independent inverse transpose');

            // The cached normal must refresh after the parent changes post-fold.
            const aabbBefore = leaf._aabbVer;
            const newParentLocal = new Mat4().setTRS(
                new Vec3(10, 0, 0),
                new Quat().setFromEulerAngles(0, 70, 0),
                new Vec3(1, 2, 3)
            );
            leaf.parent.setLocalMatrix(newParentLocal);
            root.syncHierarchy();
            check(leaf._aabbVer > aabbBefore, 'AABB version bumps when a retained parent moves');
            const normalAfter = normalMatrixRef(leaf.getWorldTransform());
            check(maxDiff(leaf.normalMatrix, normalAfter) < EPS,
                'normal matrix recomputes from the moved hierarchy');
            check(leaf.worldScaleSign === scaleSignRef(leaf.getWorldTransform()),
                'world scale sign tracks the moved hierarchy');
            check(maxDiff(leaf.getWorldTransform(), expected.get(leaf)) > EPS,
                'descendant world changes when the retained parent moves (it follows)');
        }
    },
    {
        name: 'continuous matrix updates keep descendants following and never rebuild TRS',
        run(check) {
            const scenario = buildScenario();
            const { root, a, leaf } = scenario;
            flattenStaticHierarchy(root, [scenario.p1, scenario.p2]);

            for (let i = 1; i <= 3; i++) {
                const parentWorld = new Mat4().setTRS(
                    new Vec3(i, 2 * i, -i),
                    new Quat().setFromEulerAngles(10 * i, 0, 25 * i),
                    new Vec3(1, 0.5 * i, 2)
                );
                a.setLocalMatrix(parentWorld);
                root.syncHierarchy();
                root.syncHierarchy(); // repeated sync must be a no-op

                check(a.isLocalMatrix && leaf.isLocalMatrix, 'nodes stay in matrix mode under repeated sync');
                check(maxDiff(a.getLocalTransform(), parentWorld) === 0,
                    'repeated sync never rebuilds the local matrix from TRS');

                const expectedLeaf = mul4Ref(a.getWorldTransform(), leaf.getLocalTransform());
                check(maxDiff(leaf.getWorldTransform(), expectedLeaf) < EPS,
                    `iteration ${i}: descendant follows the moved retained parent`);
            }
        }
    },
    {
        name: 'clone and copy preserve matrix mode with independent buffers',
        run(check) {
            const node = new GraphNode('matrix-node');
            const shear = new Mat4().setTRS(
                new Vec3(3, 4, 5),
                new Quat().setFromEulerAngles(0, 45, 0),
                new Vec3(2, 3, 4)
            );
            // Add shear by composing a rotated frame inside a non-uniform scale.
            const scaled = new Mat4().setScale(2, 0.5, -3);
            shear.mul2(scaled, shear);
            node.setLocalMatrix(shear);

            const clone = node.clone();
            check(clone.isLocalMatrix, 'clone inherits matrix mode');
            check(clone.localTransform.data !== node.localTransform.data, 'clone matrix is an independent buffer');
            check(maxDiff(clone.getLocalTransform(), node.getLocalTransform()) === 0, 'clone matrix values equal');
            clone.localTransform.data[0] += 10;
            check(Math.abs(node.getLocalTransform().data[0] - clone.localTransform.data[0]) > 1,
                'mutating the clone matrix leaves the original untouched');

            const copied = new GraphNode();
            copied.copy(node);
            check(copied.isLocalMatrix, 'copy inherits matrix mode');
            check(copied.localTransform.data !== node.localTransform.data, 'copy matrix is an independent buffer');
            check(maxDiff(copied.getLocalTransform(), node.getLocalTransform()) === 0, 'copy matrix values equal');

            // setLocalMatrix copies its argument, so later argument edits don't leak in.
            const arg = new Mat4().setTranslate(6, 7, 8);
            node.setLocalMatrix(arg);
            arg.data[12] = 100;
            check(node.getLocalTransform().data[12] === 6, 'setLocalMatrix takes a copy of the argument');
        }
    },
    {
        name: 'setLocalTRS returns to TRS mode and matrix-based reads stay exact',
        run(check) {
            const node = new GraphNode();
            const shear = new Mat4();
            shear.data.set([
                1, 0.2, 0, 0,
                0, 2, 0, 0,
                0.4, 0, -1, 0,
                9, 8, 7, 1
            ]);
            node.setLocalMatrix(shear);
            check(node.isLocalMatrix, 'node enters matrix mode');

            // Matrix-based reads reflect the actual sheared matrix, not a TRS approximation.
            check(node.getPosition().x === 9 && node.getPosition().y === 8 && node.getPosition().z === 7,
                'getPosition reads the matrix translation');
            check(maxDiff(node.getLocalTransform(), shear) === 0, 'getLocalTransform returns the exact matrix');
            check(maxDiff(node.getWorldTransform(), shear) === 0, 'root world equals the local matrix');
            check(typeof node.right.x === 'number' && typeof node.up.y === 'number' &&
                  typeof node.forward.z === 'number', 'direction vectors work in matrix mode');
            check(Number.isFinite(node.normalMatrix.data[0]), 'normal matrix works in matrix mode');
            check(node.worldScaleSign === -1, 'worldScaleSign reads the reflected matrix directly');

            const pos = new Vec3(1, 2, 3);
            const rot = new Quat().setFromEulerAngles(0, 90, 0);
            const scale = new Vec3(2, 2, 2);
            node.setLocalTRS(pos, rot, scale);
            check(!node.isLocalMatrix, 'setLocalTRS exits matrix mode');

            check(node.getLocalPosition().equals(pos), 'local position readable again');
            check(node.getLocalScale().equals(scale), 'local scale readable again');
            const expectedLocal = new Mat4().setTRS(pos, rot, scale);
            check(maxDiff(node.getLocalTransform(), expectedLocal) === 0, 'local transform rebuilt from TRS');

            // A node that never entered matrix mode behaves exactly as before.
            const plain = new GraphNode();
            check(plain.isLocalMatrix === false, 'fresh nodes default to TRS mode');
            plain.setLocalPosition(1, 0, 0);
            plain.setLocalRotation(0, 0, 0, 1);
            plain.setLocalScale(1, 1, 1);
            check(plain.getLocalPosition().x === 1, 'TRS APIs keep working on plain nodes');
        }
    },
    {
        name: 'illegal matrices and guarded APIs are rejected with RangeError',
        run(check) {
            const node = new GraphNode();

            const singular = new Mat4().setScale(0, 2, 2);
            expectRangeError(check, () => node.setLocalMatrix(singular), 'singular matrix');
            check(!node.isLocalMatrix, 'failed setLocalMatrix does not enter matrix mode');

            const nonAffine = new Mat4();
            nonAffine.data[3] = 0.5;
            expectRangeError(check, () => node.setLocalMatrix(nonAffine), 'non-affine matrix');

            const nonFinite = new Mat4();
            nonFinite.data[5] = Number.NaN;
            expectRangeError(check, () => node.setLocalMatrix(nonFinite), 'non-finite matrix');

            const valid = new Mat4().setTranslate(1, 2, 3);
            node.setLocalMatrix(valid);

            const guarded = [
                () => node.getLocalPosition(),
                () => node.getLocalRotation(),
                () => node.getLocalScale(),
                () => node.getLocalEulerAngles(),
                () => node.getRotation(),
                () => node.getEulerAngles(),
                () => node.setLocalPosition(1, 0, 0),
                () => node.setLocalRotation(0, 0, 0, 1),
                () => node.setLocalScale(1, 1, 1),
                () => node.setLocalEulerAngles(0, 0, 0),
                () => node.setPosition(0, 0, 0),
                () => node.setRotation(0, 0, 0, 1),
                () => node.setEulerAngles(0, 0, 0),
                () => node.setPositionAndRotation(new Vec3(), new Quat()),
                () => node.lookAt(1, 2, 3),
                () => node.translate(1, 0, 0),
                () => node.translateLocal(1, 0, 0),
                () => node.rotate(1, 0, 0),
                () => node.rotateLocal(1, 0, 0)
            ];
            for (const fn of guarded) {
                expectRangeError(check, fn, fn.name || 'guarded TRS API');
            }

            // Reads that must remain available in matrix mode.
            check(Number.isFinite(node.getPosition().x), 'getPosition stays available');
            check(node.getWorldTransform() && node.getLocalTransform(), 'matrix transforms stay available');
            check(node.right.length() > 0 && node.up.length() > 0 && node.forward.length() > 0,
                'direction vectors stay available');

            // Repeated sync keeps the exact matrix (no silent TRS rebuild).
            const before = Array.from(node.getLocalTransform().data);
            node.getWorldTransform();
            node.getWorldTransform();
            check(maxDiff(node.getLocalTransform().data, before) === 0,
                'shear is not dropped by repeated synchronization');
        }
    },
    {
        name: 'flatten rejects bad input atomically and insert events observe folded matrices',
        run(check) {
            // --- Rejection cases must leave the hierarchy untouched ---
            const base = buildScenario();
            const originalChildren = base.root.children.slice();
            const originalAChildren = base.a.children.slice();

            expectRangeError(check, () => flattenStaticHierarchy(base.root, [base.p1, base.p1]), 'duplicate pivots');
            expectRangeError(check, () => flattenStaticHierarchy(base.root, [base.root]), 'root as pivot');

            const outsider = new GraphNode('outsider');
            expectRangeError(check, () => flattenStaticHierarchy(base.root, [outsider]), 'tree-outside pivot');

            class CustomNode extends GraphNode {}
            const subClass = new CustomNode();
            base.a.addChild(subClass);
            expectRangeError(check, () => flattenStaticHierarchy(base.root, [subClass]), 'subclass pivot');
            subClass.remove();

            // A subclass anywhere below a pivot is out of scope as well.
            const nestedSub = new CustomNode();
            base.p2.addChild(nestedSub);
            expectRangeError(check, () => flattenStaticHierarchy(base.root, [base.p1]), 'subclass under pivot');
            nestedSub.remove();

            const disabled = new GraphNode('disabled');
            base.a.addChild(disabled);
            disabled._enabled = false;
            expectRangeError(check, () => flattenStaticHierarchy(base.root, [disabled]), 'disabled pivot');
            disabled.remove();

            base.leaf.scaleCompensation = true;
            expectRangeError(check, () => flattenStaticHierarchy(base.root, [base.p1]), 'scale compensation');
            base.leaf.scaleCompensation = false;

            const degenerate = buildScenario();
            degenerate.p1.setLocalScale(0, 1, 1);
            expectRangeError(check, () => flattenStaticHierarchy(degenerate.root, [degenerate.p1]), 'singular pivot matrix');

            check(base.root.children === base.root.children &&
                  base.root.children.every((c, i) => c === originalChildren[i]), 'root hierarchy unchanged after rejections');
            check(base.a.children.every((c, i) => c === originalAChildren[i]), 'pivot hierarchy unchanged after rejections');

            // Empty input is a no-op.
            const nothing = flattenStaticHierarchy(base.root, []);
            check(Array.isArray(nothing) && nothing.length === 0, 'empty pivot list is a no-op');

            // --- Insert/remove events observe the final folded hierarchy and matrices ---
            const scenario = buildScenario();
            const expected = referenceWorlds(scenario);
            const deep = new GraphNode('deep');
            deep.setLocalPosition(0.5, 0.25, -0.5);
            scenario.leaf.addChild(deep);
            const eventNodes = [];

            scenario.leaf.on('insert', (parent) => {
                check(parent === scenario.a, 'insert callback parent is the final parent');
                check(maxDiff(scenario.leaf.getWorldTransform(), expected.get(scenario.leaf)) < EPS,
                    'insert callback already sees the folded correct matrix');
                eventNodes.push('leaf-insert');
            });
            deep.on('inserthierarchy', () => {
                check(Number.isFinite(deep.getWorldTransform().data[0]),
                    'inserthierarchy callback sees a synced descendant matrix');
                const deepExpected = mul4Ref(expected.get(scenario.leaf), deep.getLocalTransform());
                check(maxDiff(deep.getWorldTransform(), deepExpected) < EPS,
                    'inserthierarchy callback sees the folded descendant matrix');
                eventNodes.push('deep-inserthierarchy');
            });
            scenario.p1.on('remove', (parent) => {
                check(parent === scenario.a, 'pivot remove callback names the old parent');
                eventNodes.push('p1-remove');
            });

            flattenStaticHierarchy(scenario.root, [scenario.p1, scenario.p2]);
            check(eventNodes.includes('leaf-insert') && eventNodes.includes('deep-inserthierarchy') &&
                  eventNodes.includes('p1-remove'), 'existing engine hierarchy events fire during the fold');
        }
    }
];

export default cases;
