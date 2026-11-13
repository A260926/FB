import { BoundingBox } from '../../src/core/shape/bounding-box.js';
import { Mat3 } from '../../src/core/math/mat3.js';
import { Mat4 } from '../../src/core/math/mat4.js';
import { Quat } from '../../src/core/math/quat.js';
import { Vec3 } from '../../src/core/math/vec3.js';
import { GraphNode } from '../../src/scene/graph-node.js';
import { flattenStaticHierarchy } from '../../src/scene/flatten-static-hierarchy.js';

const EPSILON = 1e-5;

/**
 * Asserts that two numbers are close within the required tolerance.
 *
 * @param {Function} check - The check function provided by the runner.
 * @param {number} actual - The computed value.
 * @param {number} expected - The reference value.
 * @param {string} message - Description of the value being checked.
 * @param {number} [epsilon] - Allowed absolute error.
 */
function close(check, actual, expected, message, epsilon = EPSILON) {
    check(Math.abs(actual - expected) <= epsilon, `${message}: expected ${expected}, got ${actual}`);
}

/**
 * Asserts that two matrices are element-wise close.
 *
 * @param {Function} check - The check function provided by the runner.
 * @param {Mat4} actual - The computed matrix.
 * @param {Mat4} expected - The reference matrix.
 * @param {string} message - Description of the matrices being checked.
 * @param {number} [epsilon] - Allowed absolute error.
 */
function closeMatrix(check, actual, expected, message, epsilon = EPSILON) {
    for (let i = 0; i < 16; i++) {
        close(check, actual.data[i], expected.data[i], `${message}[${i}]`, epsilon);
    }
}

/**
 * Asserts that two 3x3 matrices are element-wise close.
 *
 * @param {Function} check - The check function provided by the runner.
 * @param {Mat3} actual - The computed matrix.
 * @param {Mat3} expected - The reference matrix.
 * @param {string} message - Description of the matrices being checked.
 * @param {number} [epsilon] - Allowed absolute error.
 */
function closeMat3(check, actual, expected, message, epsilon = EPSILON) {
    for (let i = 0; i < 9; i++) {
        close(check, actual.data[i], expected.data[i], `${message}[${i}]`, epsilon);
    }
}

/**
 * Multiplies a list of matrices in order, producing `m[0] * m[1] * ...`.
 *
 * @param {...Mat4} matrices - The matrices to combine.
 * @returns {Mat4} The product.
 */
function multiply(...matrices) {
    const result = new Mat4().copy(matrices[0]);
    const temp = new Mat4();
    for (let i = 1; i < matrices.length; i++) {
        temp.mul2(result, matrices[i]);
        result.copy(temp);
    }
    return result;
}

/**
 * Builds a TRS matrix using the engine's composition convention.
 *
 * @param {Vec3} position - Translation.
 * @param {Quat} rotation - Rotation.
 * @param {Vec3} scale - Scale.
 * @returns {Mat4} The composed matrix.
 */
function trs(position, rotation, scale) {
    return new Mat4().setTRS(position, rotation, scale);
}

/**
 * Creates a node that is enabled in the hierarchy rooted at itself.
 *
 * @param {string} name - The node name.
 * @returns {GraphNode} The new node.
 */
function rootNode(name) {
    const node = new GraphNode(name);
    node._enabledInHierarchy = true;
    return node;
}

/**
 * Computes the world-space AABB of a local box by independently transforming its eight corners.
 *
 * @param {BoundingBox} box - The local-space box.
 * @param {Mat4} world - The world transform.
 * @returns {BoundingBox} The world-space AABB.
 */
function aabbFromCorners(box, world) {
    const c = box.center;
    const h = box.halfExtents;
    const result = new BoundingBox();
    let minX = Infinity;
    let minY = Infinity;
    let minZ = Infinity;
    let maxX = -Infinity;
    let maxY = -Infinity;
    let maxZ = -Infinity;

    for (let i = 0; i < 8; i++) {
        const corner = new Vec3(
            c.x + ((i & 1) ? h.x : -h.x),
            c.y + ((i & 2) ? h.y : -h.y),
            c.z + ((i & 4) ? h.z : -h.z)
        );
        const p = world.transformPoint(corner);
        minX = Math.min(minX, p.x);
        minY = Math.min(minY, p.y);
        minZ = Math.min(minZ, p.z);
        maxX = Math.max(maxX, p.x);
        maxY = Math.max(maxY, p.y);
        maxZ = Math.max(maxZ, p.z);
    }

    result.setMinMax(new Vec3(minX, minY, minZ), new Vec3(maxX, maxY, maxZ));
    return result;
}

/**
 * Transforms a vector by a Mat3.
 *
 * @param {Mat3} matrix - The matrix.
 * @param {Vec3} vector - The vector.
 * @returns {Vec3} The transformed vector.
 */
function transformVec3(matrix, vector) {
    const d = matrix.data;
    return new Vec3(
        d[0] * vector.x + d[3] * vector.y + d[6] * vector.z,
        d[1] * vector.x + d[4] * vector.y + d[7] * vector.z,
        d[2] * vector.x + d[5] * vector.y + d[8] * vector.z
    );
}

/**
 * Computes the inverse-transpose of the upper-left 3x3 of a matrix, independently of the engine's
 * `Mat3.invertMat4`.
 *
 * @param {Mat4} matrix - The 4x4 matrix.
 * @returns {Mat3} The normal matrix.
 */
function inverseTranspose(matrix) {
    const m = matrix.data;

    // upper-left 3x3, column-major input
    const a = m[0], b = m[4], c = m[8];
    const d = m[1], e = m[5], f = m[9];
    const g = m[2], h = m[6], i = m[10];

    const det = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
    const invDet = 1 / det;

    // inverse of the 3x3, row-major terms
    const i00 = (e * i - f * h) * invDet;
    const i01 = (c * h - b * i) * invDet;
    const i02 = (b * f - c * e) * invDet;
    const i10 = (f * g - d * i) * invDet;
    const i11 = (a * i - c * g) * invDet;
    const i12 = (c * d - a * f) * invDet;
    const i20 = (d * h - e * g) * invDet;
    const i21 = (b * g - a * h) * invDet;
    const i22 = (a * e - b * d) * invDet;

    // transpose into column-major Mat3 data
    const result = new Mat3();
    result.data[0] = i00;
    result.data[1] = i01;
    result.data[2] = i02;
    result.data[3] = i10;
    result.data[4] = i11;
    result.data[5] = i12;
    result.data[6] = i20;
    result.data[7] = i21;
    result.data[8] = i22;
    return result;
}

const cases = [
    {
        name: 'flattens nested pivots and preserves retained world matrices',
        run(check) {
            const root = rootNode('root');

            // A non-uniformly scaled, rotated, negatively scaled ancestor.
            const assembly = new GraphNode('assembly');
            root.addChild(assembly);
            const rootScale = new Vec3(2, 0.5, -3);
            assembly.setLocalTRS(new Vec3(1, 2, 3), new Quat().setFromEulerAngles(15, 40, 10), rootScale);

            const inner = new GraphNode('inner');
            assembly.addChild(inner);
            const innerScale = new Vec3(-1.5, 2, 0.75);
            inner.setLocalTRS(new Vec3(-2, 0.5, 1), new Quat().setFromEulerAngles(-25, 10, 65), innerScale);

            // Outer pivot: non-uniform scale plus a non-axis-aligned rotation.
            const outerPivot = new GraphNode('outerPivot');
            inner.addChild(outerPivot);
            const outerPivotMatrix = trs(
                new Vec3(0.5, -1, 2),
                new Quat().setFromEulerAngles(35, -15, 25),
                new Vec3(1.25, 0.4, 2.5)
            );
            outerPivot.setLocalMatrix(outerPivotMatrix);

            const branch = new GraphNode('branch');
            outerPivot.addChild(branch);
            const branchScale = new Vec3(0.5, 1.75, -0.8);
            branch.setLocalTRS(new Vec3(1, 0, 0.5), new Quat().setFromEulerAngles(5, 70, -20), branchScale);

            // Inner pivot, nested below the outer pivot.
            const innerPivot = new GraphNode('innerPivot');
            branch.addChild(innerPivot);
            const innerPivotMatrix = trs(
                new Vec3(0.3, 0.7, -0.4),
                new Quat().setFromEulerAngles(-50, 20, 15),
                new Vec3(0.6, 1.4, 0.9)
            );
            innerPivot.setLocalMatrix(innerPivotMatrix);

            const leaf = new GraphNode('leaf');
            innerPivot.addChild(leaf);
            const leafScale = new Vec3(1.3, 0.7, 1.1);
            leaf.setLocalTRS(new Vec3(2, -0.5, 0.25), new Quat().setFromEulerAngles(10, -35, 80), leafScale);

            const sibling = new GraphNode('sibling');
            innerPivot.addChild(sibling);
            sibling.setLocalTRS(new Vec3(-0.5, 1, 0), new Quat().setFromEulerAngles(0, 0, 0), new Vec3(1, 1, 1));

            const retained = [assembly, inner, branch, leaf, sibling, root];

            // Reference world transforms are computed independently by multiplying local
            // matrices, not by reading the engine's cached hierarchy.
            const rootWorld = root.getLocalTransform();
            const assemblyWorld = multiply(rootWorld, assembly.getLocalTransform());
            const innerWorld = multiply(assemblyWorld, inner.getLocalTransform());
            const outerPivotWorld = multiply(innerWorld, outerPivotMatrix);
            const branchWorld = multiply(outerPivotWorld, branch.getLocalTransform());
            const innerPivotWorld = multiply(branchWorld, innerPivotMatrix);
            const leafWorld = multiply(innerPivotWorld, leaf.getLocalTransform());
            const siblingWorld = multiply(innerPivotWorld, sibling.getLocalTransform());

            const expected = new Map([
                [root, rootWorld],
                [assembly, assemblyWorld],
                [inner, innerWorld],
                [branch, branchWorld],
                [leaf, leafWorld],
                [sibling, siblingWorld]
            ]);

            // Deliberately do not call getWorldTransform on these nodes before the fold: the
            // reference is built by multiplying local matrices, so the fold has to resolve the
            // latest local matrices of unsynchronized ancestors itself.

            flattenStaticHierarchy(root, [innerPivot, outerPivot]);

            // Both pivots are detached and empty; they are not destroyed.
            check(outerPivot.parent === null, 'outer pivot is detached');
            check(innerPivot.parent === null, 'inner pivot is detached');
            check(outerPivot.children.length === 0, 'outer pivot has no children');
            check(innerPivot.children.length === 0, 'inner pivot has no children');

            // Retained nodes keep their identity and their world transform.
            check(inner.parent === assembly, 'branch parent is preserved');
            check(branch.parent === inner, 'branch promoted to outer pivot parent');
            check(leaf.parent === branch, 'leaf promoted to branch');
            check(sibling.parent === branch, 'sibling promoted to branch');

            // A pivot in the middle of a sibling list hands its exact position to its children.
            const positionalRoot = new GraphNode('positionalRoot');
            const before0 = new GraphNode('before0');
            const before1 = new GraphNode('before1');
            const positionalPivot = new GraphNode('positionalPivot');
            const mid0 = new GraphNode('mid0');
            const mid1 = new GraphNode('mid1');
            const after0 = new GraphNode('after0');
            positionalRoot.addChild(before0);
            positionalRoot.addChild(before1);
            positionalRoot.addChild(positionalPivot);
            positionalRoot.addChild(after0);
            positionalPivot.addChild(mid0);
            positionalPivot.addChild(mid1);
            flattenStaticHierarchy(positionalRoot, [positionalPivot]);
            check(
                positionalRoot.children.map(node => node.name).join(',') === 'before0,before1,mid0,mid1,after0',
                `pivot position preserved: ${positionalRoot.children.map(node => node.name).join(',')}`
            );
            check(positionalPivot.parent === null && positionalPivot.children.length === 0, 'positional pivot detached and emptied');

            for (const node of retained) {
                closeMatrix(check, node.getWorldTransform(), expected.get(node), `${node.name} world transform`);
            }

            // The fold is order independent: feed the pivots in the opposite order.
            const root2 = rootNode('root2');
            const a2 = new GraphNode('assembly');
            root2.addChild(a2);
            a2.setLocalTRS(new Vec3(1, 2, 3), new Quat().setFromEulerAngles(15, 40, 10), rootScale);
            const i2 = new GraphNode('inner');
            a2.addChild(i2);
            i2.setLocalTRS(new Vec3(-2, 0.5, 1), new Quat().setFromEulerAngles(-25, 10, 65), innerScale);
            const op2 = new GraphNode('outerPivot');
            i2.addChild(op2);
            op2.setLocalMatrix(outerPivotMatrix);
            const b2 = new GraphNode('branch');
            op2.addChild(b2);
            b2.setLocalTRS(new Vec3(1, 0, 0.5), new Quat().setFromEulerAngles(5, 70, -20), branchScale);
            const ip2 = new GraphNode('innerPivot');
            b2.addChild(ip2);
            ip2.setLocalMatrix(innerPivotMatrix);
            const leaf2 = new GraphNode('leaf');
            ip2.addChild(leaf2);
            leaf2.setLocalTRS(new Vec3(2, -0.5, 0.25), new Quat().setFromEulerAngles(10, -35, 80), leafScale);
            const sib2 = new GraphNode('sibling');
            ip2.addChild(sib2);
            sib2.setLocalTRS(new Vec3(-0.5, 1, 0), new Quat(), new Vec3(1, 1, 1));

            flattenStaticHierarchy(root2, [ip2, op2]);
            closeMatrix(check, leaf2.getWorldTransform(), leafWorld, 'order independent leaf world');
            closeMatrix(check, sib2.getWorldTransform(), siblingWorld, 'order independent sibling world');
            check(b2.children[0] === leaf2 && b2.children[1] === sib2, 'order independent child ordering');

            // Traversal order of retained nodes is preserved.
            const afterOrder = root.find(() => true).map(node => node.name);
            check(afterOrder.join(',') === 'root,assembly,inner,branch,leaf,sibling', `retained pre-order ${afterOrder.join(',')}`);

            // Moving a retained parent still moves its descendants.
            const leafWorldBefore = leaf.getWorldTransform().clone();
            check(inner.isLocalMatrix === false, 'retained TRS ancestor stays in TRS mode');
            inner.setLocalPosition(inner.getLocalPosition().x + 1, 0, 0);
            closeMatrix(check, leaf.getWorldTransform(), multiply(inner.getWorldTransform(), branch.getLocalTransform(), leaf.getLocalTransform()), 'descendant follows moved parent');
            check(!leaf.getWorldTransform().equals(leafWorldBefore), 'descendant world changed when parent moved');
        }
    },
    {
        name: 'flattens pivots under unsynchronized ancestors',
        run(check) {
            const root = rootNode('root');
            const parent = new GraphNode('parent');
            root.addChild(parent);
            parent.setLocalTRS(new Vec3(4, -2, 1), new Quat().setFromEulerAngles(0, 55, 0), new Vec3(1.5, 1, 2));

            const pivot = new GraphNode('pivot');
            parent.addChild(pivot);
            pivot.setLocalTRS(new Vec3(1, 2, 3), new Quat().setFromEulerAngles(20, 0, 35), new Vec3(1, 3, 1));

            const child = new GraphNode('child');
            pivot.addChild(child);
            child.setLocalTRS(new Vec3(-1, 0.5, 2), new Quat().setFromEulerAngles(0, 0, 15), new Vec3(2, 1, 0.5));

            // No getWorldTransform calls before the fold.
            const expected = multiply(root.getLocalTransform(), parent.getLocalTransform(), pivot.getLocalTransform(), child.getLocalTransform());

            flattenStaticHierarchy(root, [pivot]);

            closeMatrix(check, child.getWorldTransform(), expected, 'unsynchronized ancestor world');
            check(child.parent === parent, 'child promoted under unsynchronized parent');
            check(child.isLocalMatrix, 'promoted child uses local matrix mode');
        }
    },
    {
        name: 'world bounds and normal matrix come from the matrix',
        run(check) {
            const root = rootNode('root');

            const pivot = new GraphNode('pivot');
            root.addChild(pivot);
            pivot.setLocalTRS(new Vec3(1, 0, 0), new Quat().setFromEulerAngles(0, 30, 0), new Vec3(2, 0.5, 1));

            const child = new GraphNode('child');
            pivot.addChild(child);
            // A non-uniformly scaled parent combined with a rotated child produces shear.
            child.setLocalTRS(new Vec3(0.5, 1, -0.5), new Quat().setFromEulerAngles(40, 20, 10), new Vec3(1, 2, 1));
            const worldBefore = child.getWorldTransform().clone();
            const localBox = new BoundingBox(new Vec3(0.25, -0.5, 0.75), new Vec3(1, 0.5, 2));
            const expectedAabb = aabbFromCorners(localBox, worldBefore);

            flattenStaticHierarchy(root, [pivot]);

            const world = child.getWorldTransform();
            check(child.isLocalMatrix, 'child is in matrix mode after flatten');

            // The engine's AABB path must match the independent eight-corner computation.
            const engineAabb = new BoundingBox();
            engineAabb.setFromTransformedAabb(localBox, world);
            close(check, engineAabb.center.x, expectedAabb.center.x, 'aabb center x');
            close(check, engineAabb.center.y, expectedAabb.center.y, 'aabb center y');
            close(check, engineAabb.center.z, expectedAabb.center.z, 'aabb center z');
            close(check, engineAabb.halfExtents.x, expectedAabb.halfExtents.x, 'aabb half x');
            close(check, engineAabb.halfExtents.y, expectedAabb.halfExtents.y, 'aabb half y');
            close(check, engineAabb.halfExtents.z, expectedAabb.halfExtents.z, 'aabb half z');

            // Normals must be transformed with the inverse transpose of the actual world matrix.
            const referenceNormalMatrix = inverseTranspose(world);
            const normal = child.normalMatrix;
            const localNormal = new Vec3(0.3, 0.8, -0.5).normalize();
            const expectedNormal = transformVec3(referenceNormalMatrix, localNormal).normalize();
            const actualNormal = transformVec3(normal, localNormal).normalize();
            close(check, actualNormal.x, expectedNormal.x, 'normal x');
            close(check, actualNormal.y, expectedNormal.y, 'normal y');
            close(check, actualNormal.z, expectedNormal.z, 'normal z');

            // The world scale sign must reflect the actual matrix (negative scale from the root).
            const reflection = new GraphNode('reflection');
            root.addChild(reflection);
            reflection.setLocalScale(-1, 1, 1);
            check(reflection.worldScaleSign === -1, 'negative scale sign reflected');
        }
    },
    {
        name: 'matrix mode round trips, clones and rejects invalid input',
        run(check) {
            const node = rootNode('node');
            const shear = new Mat4().set([
                1, 0, 0, 0,
                0.5, 1, 0, 0,
                0, 0.25, 1, 0,
                2, 3, 4, 1
            ]);

            node.setLocalMatrix(shear);
            check(node.isLocalMatrix === true, 'enters matrix mode');
            closeMatrix(check, node.getLocalTransform(), shear, 'local transform is the matrix');

            const position = node.getPosition();
            close(check, position.x, 2, 'world position x');
            close(check, position.y, 3, 'world position y');
            close(check, position.z, 4, 'world position z');

            // Direction vectors come from the matrix, not a stale TRS.
            const expectedX = new Vec3(shear.data[0], shear.data[1], shear.data[2]).normalize();
            close(check, node.right.x, expectedX.x, 'right x');

            // Repeated synchronization must not re-decompose into a TRS.
            for (let i = 0; i < 5; i++) {
                node.getWorldTransform();
                node.syncHierarchy();
            }
            closeMatrix(check, node.getLocalTransform(), shear, 'matrix survives repeated sync');

            // A repeated setLocalMatrix updates the cached world and bounds version.
            const aabbVer = node._aabbVer;
            const updated = new Mat4().copy(shear).mul(new Mat4().setTranslate(1, 0, 0));            node.setLocalMatrix(updated);
            check(node._aabbVer > aabbVer, 'aabb version bumps on matrix update');
            closeMatrix(check, node.getWorldTransform(), updated, 'world follows repeated set');

            // Clone copies mode and matrix with independent buffers.
            const clone = node.clone();
            check(clone.isLocalMatrix === true, 'clone keeps matrix mode');
            closeMatrix(check, clone.getLocalTransform(), updated, 'clone copies matrix');
            check(clone.localTransform !== node.localTransform, 'clone has its own matrix buffer');
            check(clone.localTransform.data !== node.localTransform.data, 'clone has its own matrix data');

            // Modifying the clone matrix doesn't change the source.
            clone.setLocalMatrix(new Mat4().setTranslate(9, 9, 9));
            closeMatrix(check, node.getLocalTransform(), updated, 'clone matrix is independent');

            // Switching back to TRS requires the full TRS.
            const newPosition = new Vec3(5, 6, 7);
            const newRotation = new Quat().setFromEulerAngles(10, 20, 30);
            const newScale = new Vec3(2, 3, 4);
            node.setLocalTRS(newPosition, newRotation, newScale);
            check(node.isLocalMatrix === false, 'leaves matrix mode');
            check(node.getLocalPosition().equals(newPosition), 'trs position restored');
            check(node.getLocalRotation().equals(newRotation), 'trs rotation restored');
            check(node.getLocalScale().equals(newScale), 'trs scale restored');
            closeMatrix(check, node.getWorldTransform(), trs(newPosition, newRotation, newScale), 'world follows TRS');

            // Rejection of invalid matrices leaves the node untouched.
            const guiBefore = node.getLocalTransform().clone();
            const rejects = [
                [new Mat4().set([1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 2]), 'non-affine'],
                [new Mat4().set([1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1]), 'non-invertible'],
                [new Mat4().set([NaN, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1]), 'non-finite']
            ];
            for (const [matrix, label] of rejects) {
                let error = null;
                try {
                    node.setLocalMatrix(matrix);
                } catch (e) {
                    error = e;
                }
                check(error instanceof RangeError, `${label} matrix rejected with RangeError`);
            }
            closeMatrix(check, node.getLocalTransform(), guiBefore, 'local transform unchanged after rejection');

            let typeError = null;
            try {
                node.setLocalMatrix([1, 0, 0, 0]);
            } catch (e) {
                typeError = e;
            }
            check(typeError instanceof TypeError, 'non-Mat4 rejected with TypeError');

            // TRS-based operations throw RangeError in matrix mode.
            node.setLocalMatrix(shear);
            const trsOperations = [
                () => node.getLocalPosition(),
                () => node.getLocalRotation(),
                () => node.getLocalScale(),
                () => node.getLocalEulerAngles(),
                () => node.setLocalPosition(1, 2, 3),
                () => node.setLocalRotation(new Quat()),
                () => node.setLocalScale(1, 1, 1),
                () => node.setLocalEulerAngles(1, 2, 3),
                () => node.setPosition(1, 2, 3),
                () => node.setRotation(new Quat()),
                () => node.setPositionAndRotation(new Vec3(), new Quat()),
                () => node.setEulerAngles(1, 2, 3),
                () => node.getRotation(),
                () => node.getEulerAngles(),
                () => node.translate(1, 0, 0),
                () => node.translateLocal(1, 0, 0),
                () => node.rotate(1, 0, 0),
                () => node.rotateLocal(1, 0, 0),
                () => node.lookAt(0, 0, 0)
            ];
            for (let i = 0; i < trsOperations.length; i++) {
                let error = null;
                try {
                    trsOperations[i]();
                } catch (e) {
                    error = e;
                }
                check(error instanceof RangeError, `TRS operation ${i} throws RangeError in matrix mode`);
            }

            // TRS mode nodes are unaffected.
            const plain = rootNode('plain');
            check(plain.isLocalMatrix === false, 'plain node stays in TRS mode');
            plain.setLocalPosition(1, 2, 3);
            check(plain.getLocalPosition().x === 1, 'plain node still allows TRS edits');

            // Copy carries the mode.
            const source = rootNode('source');
            source.setLocalMatrix(shear);
            const destination = rootNode('destination');
            destination.copy(source);
            check(destination.isLocalMatrix === true, 'copy keeps matrix mode');
            closeMatrix(check, destination.getLocalTransform(), shear, 'copy carries matrix');
            check(destination.localTransform.data !== source.localTransform.data, 'copy has independent data');
        }
    },
    {
        name: 'flatten rejects unsupported hierarchies and exposes correct state in events',
        run(check) {
            const root = rootNode('root');

            // Root itself is not a valid pivot.
            let rootError = null;
            try {
                flattenStaticHierarchy(root, [root]);
            } catch (e) {
                rootError = e;
            }
            check(rootError instanceof RangeError, 'root pivot rejected');

            // A root that is not a GraphNode, and pivots that are not an array.
            let badRootError = null;
            try {
                flattenStaticHierarchy({}, []);
            } catch (e) {
                badRootError = e;
            }
            check(badRootError instanceof TypeError, 'non-GraphNode root rejected');

            let badPivotsError = null;
            try {
                flattenStaticHierarchy(root, 'pivots');
            } catch (e) {
                badPivotsError = e;
            }
            check(badPivotsError instanceof TypeError, 'non-array pivots rejected');

            // An invalid matrix anywhere in the participating subtree rejects the whole fold
            // before anything is moved.
            const invalidPivot = new GraphNode('invalidPivot');
            root.addChild(invalidPivot);
            const invalidChild = new GraphNode('invalidChild');
            invalidPivot.addChild(invalidChild);
            const invalidGrandchild = new GraphNode('invalidGrandchild');
            invalidChild.addChild(invalidGrandchild);
            // Inject a singular local matrix directly, bypassing the public validation.
            invalidGrandchild.localTransform.set([1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1]);
            invalidGrandchild._localMatrixMode = true;
            invalidGrandchild._dirtyLocal = false;
            let invalidMatrixError = null;
            try {
                flattenStaticHierarchy(root, [invalidPivot]);
            } catch (e) {
                invalidMatrixError = e;
            }
            check(invalidMatrixError instanceof RangeError, 'invalid subtree matrix rejected');
            check(invalidPivot.children.length === 1 && invalidChild.children.length === 1, 'invalid matrix rejection did not modify the tree');

            // Out-of-tree pivot.
            const stranger = new GraphNode('stranger');
            let treeError = null;
            try {
                flattenStaticHierarchy(root, [stranger]);
            } catch (e) {
                treeError = e;
            }
            check(treeError instanceof RangeError, 'out-of-tree pivot rejected');

            // Duplicate pivot.
            const duplicate = new GraphNode('duplicate');
            root.addChild(duplicate);
            let duplicateError = null;
            try {
                flattenStaticHierarchy(root, [duplicate, duplicate]);
            } catch (e) {
                duplicateError = e;
            }
            check(duplicateError instanceof RangeError, 'duplicate pivot rejected');
            check(duplicate.parent === root, 'duplicate rejection did not modify the hierarchy');

            // Disabled pivot.
            const disabled = new GraphNode('disabled');
            root.addChild(disabled);
            disabled._enabled = false;
            let disabledError = null;
            try {
                flattenStaticHierarchy(root, [disabled]);
            } catch (e) {
                disabledError = e;
            }
            check(disabledError instanceof RangeError, 'disabled pivot rejected');
            check(disabled.parent === root, 'disabled rejection did not modify the hierarchy');

            // A pivot that is enabled locally but disabled through its ancestors is not enabled
            // in the live hierarchy and is rejected.
            const outerDisabled = new GraphNode('outerDisabled');
            root.addChild(outerDisabled);
            outerDisabled._enabled = false;
            outerDisabled._enabledInHierarchy = false;
            const innerEnabled = new GraphNode('innerEnabled');
            outerDisabled.addChild(innerEnabled);
            check(innerEnabled._enabled === true, 'inner node local enabled is true');
            check(innerEnabled.enabled === false, 'inner node is disabled through its ancestor');
            let hierarchyDisabledError = null;
            try {
                flattenStaticHierarchy(root, [innerEnabled]);
            } catch (e) {
                hierarchyDisabledError = e;
            }
            check(hierarchyDisabledError instanceof RangeError, 'pivot disabled through ancestors rejected');
            check(innerEnabled.parent === outerDisabled, 'hierarchy-disabled rejection did not modify the tree');

            // scaleCompensation in the participating subtree.
            const compensated = new GraphNode('compensated');
            root.addChild(compensated);
            const compensatedChild = new GraphNode('compensatedChild');
            compensated.addChild(compensatedChild);
            compensatedChild.scaleCompensation = true;
            let compensationError = null;
            try {
                flattenStaticHierarchy(root, [compensated]);
            } catch (e) {
                compensationError = e;
            }
            check(compensationError instanceof RangeError, 'scaleCompensation subtree rejected');
            check(compensated.children.length === 1, 'compensation rejection did not modify the hierarchy');

            // scaleCompensation on an ancestor between the pivot and the root is fine as long as
            // the participating pivot subtree itself does not use it: the promoted child is
            // uncompensated, so its world is parentWorld * childLocal both before and after.
            const compensatedAncestor = new GraphNode('compensatedAncestor');
            root.addChild(compensatedAncestor);
            compensatedAncestor._enabledInHierarchy = true;
            compensatedAncestor.scaleCompensation = true;
            const underCompensated = new GraphNode('underCompensated');
            compensatedAncestor.addChild(underCompensated);
            underCompensated.setLocalTRS(new Vec3(0.5, 1, 0), new Quat().setFromEulerAngles(0, 0, 30), new Vec3(1, 2, 1));
            const compensatedBox = new GraphNode('compensatedBox');
            underCompensated.addChild(compensatedBox);
            compensatedBox.setLocalTRS(new Vec3(1, 0, 0.5), new Quat(), new Vec3(1, 1, 1));
            const compensatedBoxWorld = compensatedBox.getWorldTransform().clone();
            flattenStaticHierarchy(root, [underCompensated]);
            closeMatrix(check, compensatedBox.getWorldTransform(), compensatedBoxWorld, 'fold under compensated ancestor preserves world');
            check(compensatedBox.parent === compensatedAncestor, 'promoted under compensated ancestor');

            // Subclass pivot.
            class CustomNode extends GraphNode {}
            const custom = new CustomNode('custom');
            root.addChild(custom);
            let subclassError = null;
            try {
                flattenStaticHierarchy(root, [custom]);
            } catch (e) {
                subclassError = e;
            }
            check(subclassError instanceof TypeError, 'subclass pivot rejected');

            // A valid fold under a frozen ancestor: the insert event must observe already correct
            // transforms for retained nodes and their descendants.
            const parent = new GraphNode('parent');
            root.addChild(parent);
            parent.setLocalTRS(new Vec3(1, 2, 3), new Quat().setFromEulerAngles(0, 20, 0), new Vec3(2, 0.5, 1));

            const pivot = new GraphNode('pivot');
            parent.addChild(pivot);
            pivot.setLocalTRS(new Vec3(0.5, -1, 2), new Quat().setFromEulerAngles(15, 35, 0), new Vec3(1, 1.5, 0.75));

            const child = new GraphNode('child');
            pivot.addChild(child);
            child.setLocalTRS(new Vec3(1, 0, -1), new Quat().setFromEulerAngles(0, 0, 45), new Vec3(1.25, 1, 0.5));

            const grandchild = new GraphNode('grandchild');
            child.addChild(grandchild);
            grandchild.setLocalTRS(new Vec3(0.25, 0.5, 0), new Quat(), new Vec3(1, 1, 1));

            // Freeze the hierarchy as the engine does after a sync.
            root.syncHierarchy();
            check(root._frozen === true, 'root is frozen after sync');
            check(parent._frozen === true, 'parent is frozen after sync');

            const expectedChildWorld = child.getWorldTransform().clone();
            const expectedGrandchildWorld = grandchild.getWorldTransform().clone();

            let insertParent = null;
            let insertWorld = null;
            let insertDescendantWorld = null;
            let insertDescendantNormal = null;
            let childAabbVer = 0;
            let grandchildAabbVer = 0;

            const onInsert = (eventParent) => {
                insertParent = eventParent;
                insertWorld = child.getWorldTransform().clone();
                insertDescendantWorld = grandchild.getWorldTransform().clone();
                insertDescendantNormal = grandchild.normalMatrix.clone();
                childAabbVer = child._aabbVer;
                grandchildAabbVer = grandchild._aabbVer;
            };
            child.on('insert', onInsert);

            flattenStaticHierarchy(root, [pivot]);

            check(insertParent === parent, 'insert event fired for promoted node with new parent');
            closeMatrix(check, insertWorld, expectedChildWorld, 'insert event world transform');
            closeMatrix(check, insertDescendantWorld, expectedGrandchildWorld, 'insert event descendant world');
            closeMat3(check, insertDescendantNormal, inverseTranspose(expectedGrandchildWorld), 'insert event descendant normal matrix');
            check(childAabbVer > 0 && grandchildAabbVer > 0, 'insert event sees bumped aabb versions');

            // The frozen ancestor is unfrozen and re-syncs.
            check(root._frozen === false, 'frozen ancestor unfrozen by flatten');
            root.syncHierarchy();
            closeMatrix(check, child.getWorldTransform(), expectedChildWorld, 'post-sync child world');
            closeMatrix(check, grandchild.getWorldTransform(), expectedGrandchildWorld, 'post-sync grandchild world');

            // Subsequent movement of the retained parent propagates.
            parent.setLocalPosition(10, 0, 0);
            root.syncHierarchy();
            closeMatrix(check, grandchild.getWorldTransform(), multiply(parent.getWorldTransform(), child.getLocalTransform(), grandchild.getLocalTransform()), 'moved parent propagates to descendants');

            // local enabled and tags are unchanged for retained nodes.
            const tagged = new GraphNode('tagged');
            parent.addChild(tagged);
            tagged.tags.add('alpha');
            const enabledChild = new GraphNode('enabledChild');
            tagged.addChild(enabledChild);
            enabledChild._enabled = false;
            const taggedPivot = new GraphNode('taggedPivot');
            tagged.addChild(taggedPivot);
            const survivor = new GraphNode('survivor');
            taggedPivot.addChild(survivor);
            survivor.tags.add('beta');

            flattenStaticHierarchy(root, [taggedPivot]);
            check(survivor.parent === tagged, 'tagged child promoted');
            check(survivor.tags.has('beta'), 'survivor tags preserved');
            check(enabledChild._enabled === false, 'sibling local enabled preserved');
            check(survivor._enabled === true, 'survivor local enabled preserved');
        }
    }
];

export default cases;
