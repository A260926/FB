import assert from 'node:assert/strict';

import { Mat4 } from '../../src/core/math/mat4.js';
import { Quat } from '../../src/core/math/quat.js';
import { Vec3 } from '../../src/core/math/vec3.js';
import { GraphNode } from '../../src/scene/graph-node.js';

// Run with: node test/scene/reparent-preserving-world.mjs

const RELATIVE_TOLERANCE = 1e-5;

let checks = 0;

function check(value, message) {
    assert.ok(value, message);
    checks++;
}

function assertClose(actual, expected, message, tolerance = RELATIVE_TOLERANCE) {
    const tol = tolerance * Math.max(1, Math.abs(expected));
    assert.ok(Math.abs(actual - expected) <= tol,
        `${message}: expected ${expected}, got ${actual} (tol ${tol})`);
    checks++;
}

function assertMat4Close(actual, expected, label) {
    const a = actual.data;
    const e = expected.data;
    assert.equal(a.length, 16);
    for (let i = 0; i < 16; i++) {
        const tol = RELATIVE_TOLERANCE * Math.max(1, Math.abs(e[i]));
        assert.ok(Math.abs(a[i] - e[i]) <= tol,
            `${label} element ${i}: expected ${e[i]}, got ${a[i]} (tol ${tol})`);
        checks++;
    }
}

function snapshotWorld(node) {
    return new Mat4().copy(node.getWorldTransform());
}

function makeNode(name, position, rotation, scale) {
    const node = new GraphNode(name);
    if (position) {
        node.setLocalPosition(position);
    }
    if (rotation) {
        node.setLocalRotation(rotation);
    }
    if (scale) {
        node.setLocalScale(scale);
    }
    return node;
}

// ---------------------------------------------------------------------------
// 1. Baseline from the spec: the old reparent keeps local TRS and jumps in
// world space; reparentPreservingWorld keeps the full world transform.
// ---------------------------------------------------------------------------
{
    const parentA = makeNode('parentA', new Vec3(10, 0, 0), null, new Vec3(2, 2, 2));
    const parentB = makeNode('parentB', new Vec3(-4, 0, 0), null, new Vec3(3, 3, 3));
    const child = makeNode('child', new Vec3(1, 0, 0));
    parentA.addChild(child);

    assertClose(child.getPosition().x, 12, 'child starts at world x 12');
    assertClose(child.getLocalPosition().x, 1, 'child starts at local x 1');

    // The legacy interface preserves local TRS, so the world position jumps.
    child.reparent(parentB);
    assertClose(child.getPosition().x, -1, 'legacy reparent moves world x to -1');
    assertClose(child.getLocalPosition().x, 1, 'legacy reparent keeps local x 1');
}

{
    const parentA = makeNode('parentA', new Vec3(10, 0, 0), null, new Vec3(2, 2, 2));
    const parentB = makeNode('parentB', new Vec3(-4, 0, 0), null, new Vec3(3, 3, 3));
    const child = makeNode('child', new Vec3(1, 0, 0));
    parentA.addChild(child);

    assertClose(child.getPosition().x, 12, 'child starts at world x 12');

    const returned = child.reparentPreservingWorld(parentB);
    assert.strictEqual(returned, child, 'reparentPreservingWorld returns this');
    checks++;

    assertClose(child.getPosition().x, 12, 'world x stays 12 after world-preserving reparent');
    assertClose(child.getLocalPosition().x, 16 / 3, 'local x becomes 16/3 under the new parent');
    check(child.parent === parentB, 'child is attached to parentB');
    check(parentA.children.length === 0 && parentB.children[0] === child, 'child lists moved');
}

// ---------------------------------------------------------------------------
// 2. The whole subtree keeps its world matrices; descendants keep local TRS.
// ---------------------------------------------------------------------------
{
    // Non-uniform scales stay shear-free while every basis axis is aligned;
    // the rotation coverage lives on the descendant instead.
    const parentA = makeNode('parentA', new Vec3(10, 0, 0), null, new Vec3(2, 3, 4));
    const parentB = makeNode('parentB', new Vec3(-4, 7, -2), null, new Vec3(0.5, 2, 1.5));
    const child = makeNode('child', new Vec3(1, 2, 3), null, new Vec3(1.5, 0.75, 2));
    const grandChild = makeNode('grandChild', new Vec3(-2, 1, 0.5),
        new Quat().setFromEulerAngles(0, 90, 0), new Vec3(2, 2, 2));
    const greatGrandChild = makeNode('greatGrandChild', new Vec3(0, 0, 1),
        new Quat().setFromEulerAngles(12, -34, 56), new Vec3(1, 2, 3));
    parentA.addChild(child);
    child.addChild(grandChild);
    grandChild.addChild(greatGrandChild);

    const childWorldBefore = snapshotWorld(child);
    const grandWorldBefore = snapshotWorld(grandChild);
    const greatWorldBefore = snapshotWorld(greatGrandChild);

    const grandLocalPos = grandChild.getLocalPosition().clone();
    const grandLocalRot = grandChild.getLocalRotation().clone();
    const grandLocalScale = grandChild.getLocalScale().clone();
    const greatLocalPos = greatGrandChild.getLocalPosition().clone();

    child.reparentPreservingWorld(parentB);

    assertMat4Close(child.getWorldTransform(), childWorldBefore, 'child world matrix');
    assertMat4Close(grandChild.getWorldTransform(), grandWorldBefore, 'grandchild world matrix');
    assertMat4Close(greatGrandChild.getWorldTransform(), greatWorldBefore, 'great-grandchild world matrix');

    check(grandChild.getLocalPosition().equals(grandLocalPos), 'grandchild local position untouched');
    check(grandChild.getLocalRotation().equals(grandLocalRot), 'grandchild local rotation untouched');
    check(grandChild.getLocalScale().equals(grandLocalScale), 'grandchild local scale untouched');
    check(greatGrandChild.getLocalPosition().equals(greatLocalPos), 'great-grandchild local position untouched');
}

// ---------------------------------------------------------------------------
// 3. Negative scale / reflection and non-uniform scale are supported.
// ---------------------------------------------------------------------------
{
    const parentA = makeNode('parentA', new Vec3(3, -1, 2), null, new Vec3(2, 2, 2));
    const parentB = makeNode('parentB', new Vec3(-5, 4, 1),
        new Quat().setFromEulerAngles(0, 45, 0), new Vec3(-3, 3, -3));
    const child = makeNode('child', new Vec3(1, -2, 0.5),
        new Quat().setFromEulerAngles(30, 0, -60), new Vec3(1, 1, 1));
    parentA.addChild(child);

    const worldBefore = snapshotWorld(child);
    child.reparentPreservingWorld(parentB);
    assertMat4Close(child.getWorldTransform(), worldBefore, 'reflected reparent world matrix');
    check(child.worldScaleSign === worldBefore.scaleSign, 'world reflection sign preserved');

    // The rebuilt local matrix, which may allocate the negative scale on any
    // axis, must reproduce the exact candidate local transform.
    const rebuiltLocal = new Mat4().setTRS(child.getLocalPosition(), child.getLocalRotation(), child.getLocalScale());
    const expectedLocal = new Mat4().mul2(
        new Mat4().copy(parentB.getWorldTransform()).invert(),
        worldBefore
    );
    assertMat4Close(rebuiltLocal, expectedLocal, 'rebuilt local TRS matrix');
}

{
    const parentA = makeNode('parentA', null, null, new Vec3(2, 2, 2));
    // A uniformly scaled parent keeps a rotated, non-uniformly scaled child shear-free.
    const parentB = makeNode('parentB', new Vec3(1, 1, 1), null, new Vec3(4, 4, 4));
    const child = makeNode('child', new Vec3(1, 1, 1),
        new Quat().setFromEulerAngles(15, 25, 35), new Vec3(1.5, -2, 3));
    parentA.addChild(child);

    const worldBefore = snapshotWorld(child);
    child.reparentPreservingWorld(parentB);
    assertMat4Close(child.getWorldTransform(), worldBefore, 'non-uniform scale world matrix');
}

// ---------------------------------------------------------------------------
// 4. Old parent, new parent and higher ancestors with unsynced TRS changes.
// ---------------------------------------------------------------------------
{
    const grandA = makeNode('grandA');
    const parentA = makeNode('parentA', new Vec3(10, 0, 0), null, new Vec3(2, 2, 2));
    const grandB = makeNode('grandB');
    const parentB = makeNode('parentB', new Vec3(-4, 0, 0), null, new Vec3(3, 3, 3));
    grandA.addChild(parentA);
    grandB.addChild(parentB);

    const child = makeNode('child', new Vec3(1, 0, 0));
    parentA.addChild(child);
    child.getWorldTransform(); // sync everything first

    // Now change TRS at every ancestor level without syncHierarchy.
    grandA.setLocalPosition(1, 2, 3);
    parentA.setLocalPosition(11, 0, 0);
    parentA.setLocalScale(2.5, 2.5, 2.5);
    grandB.setLocalPosition(-3, 0, 0);
    parentB.setLocalPosition(-4, 5, 0);
    parentB.setLocalScale(3, 4, 5);

    check(child._dirtyWorld === true, 'child world transform is dirty before the call');
    check(parentB._dirtyWorld === true, 'new parent world transform is dirty before the call');

    // Independently compute the expected world matrix from the same TRS values.
    const grandAMat = new Mat4().setTRS(grandA.getLocalPosition(), new Quat(), new Vec3(1, 1, 1));
    const parentAMat = new Mat4().setTRS(new Vec3(11, 0, 0), new Quat(), new Vec3(2.5, 2.5, 2.5));
    const childLocalMat = new Mat4().setTRS(new Vec3(1, 0, 0), new Quat(), new Vec3(1, 1, 1));
    const expectedWorld = new Mat4().mul2(new Mat4().mul2(grandAMat, parentAMat), childLocalMat);

    child.reparentPreservingWorld(parentB);
    assertMat4Close(child.getWorldTransform(), expectedWorld, 'unsynced ancestors world matrix');

    const grandBMat = new Mat4().setTRS(new Vec3(-3, 0, 0), new Quat(), new Vec3(1, 1, 1));
    const parentBMat = new Mat4().setTRS(new Vec3(-4, 5, 0), new Quat(), new Vec3(3, 4, 5));
    const expectedNewParentWorld = new Mat4().mul2(grandBMat, parentBMat);
    const expectedLocal = new Mat4().mul2(
        new Mat4().copy(expectedNewParentWorld).invert(),
        expectedWorld
    );
    const rebuiltLocal = new Mat4().setTRS(child.getLocalPosition(), child.getLocalRotation(), child.getLocalScale());
    assertMat4Close(rebuiltLocal, expectedLocal, 'unsynced ancestors local matrix');
}

// ---------------------------------------------------------------------------
// 5. Detaching to the root (parent === null) promotes the world matrix.
// ---------------------------------------------------------------------------
{
    const parent = makeNode('parent', new Vec3(8, -3, 2),
        new Quat().setFromEulerAngles(0, 60, 0), new Vec3(2, 2, 2));
    const child = makeNode('child', new Vec3(1, 1, 1),
        new Quat().setFromEulerAngles(45, 0, 0), new Vec3(2, 3, 4));
    parent.addChild(child);

    const worldBefore = snapshotWorld(child);
    child.reparentPreservingWorld(null);

    check(child.parent === null, 'child is detached to the root');
    assertMat4Close(child.getWorldTransform(), worldBefore, 'detached child keeps world matrix');
    assertMat4Close(child.getLocalTransform(), worldBefore, 'detached local matrix equals old world matrix');
}

// ---------------------------------------------------------------------------
// 6. Same-parent reordering keeps the transform and follows reparent index.
// ---------------------------------------------------------------------------
{
    const parent = makeNode('parent', new Vec3(5, 0, 0), null, new Vec3(2, 2, 2));
    const first = makeNode('first');
    const child = makeNode('child', new Vec3(1, 0, 0));
    const third = makeNode('third');
    parent.addChild(first);
    parent.addChild(child);
    parent.addChild(third);

    const worldBefore = snapshotWorld(child);
    const localXBefore = child.getLocalPosition().x;

    const returned = child.reparentPreservingWorld(parent, 0);
    assert.strictEqual(returned, child, 'same-parent reorder returns this');
    checks++;

    check(parent.children[0] === child, 'child moved to index 0');
    check(parent.children[1] === first && parent.children[2] === third, 'siblings shifted');
    assertMat4Close(child.getWorldTransform(), worldBefore, 'reordered child world matrix');
    assertClose(child.getLocalPosition().x, localXBefore, 'reordered child local x');

    // Default (no index) appends, matching reparent semantics.
    child.reparentPreservingWorld(parent);
    check(parent.children[2] === child, 'child appended without an index');
    assertMat4Close(child.getWorldTransform(), worldBefore, 're-appended child world matrix');
}

// ---------------------------------------------------------------------------
// 7. Shear and non-finite candidate local matrices are rejected up front.
// ---------------------------------------------------------------------------
{
    const oldParent = makeNode('oldParent');
    const newParent = makeNode('newParent');
    const child = makeNode('child', new Vec3(1, 2, 3));
    oldParent.addChild(child);
    child.getWorldTransform();

    // Inject shear directly into the synced world matrix cache. GraphNode
    // stores only TRS, so a shear guard can only be exercised this way.
    const sheared = new Mat4().copy(child.worldTransform);
    sheared.data[1] += 0.5;
    child.worldTransform.copy(sheared);
    check(child._dirtyWorld === false, 'sheared cache is treated as synced');

    let events = 0;
    const countEvent = () => events++;
    child.on('remove', countEvent);
    child.on('insert', countEvent);
    oldParent.on('childremove', countEvent);
    newParent.on('childinsert', countEvent);

    const localPosBefore = child.getLocalPosition().clone();
    assert.throws(() => child.reparentPreservingWorld(newParent), RangeError);
    checks++;

    check(child.parent === oldParent, 'shear rejection keeps the old parent');
    check(oldParent.children[0] === child && newParent.children.length === 0, 'shear rejection keeps child order');
    check(child.getLocalPosition().equals(localPosBefore), 'shear rejection keeps local TRS');
    check(events === 0, 'shear rejection fires no remove/insert events');

    // Non-finite elements are rejected the same way.
    child.worldTransform.data[5] = Infinity;
    assert.throws(() => child.reparentPreservingWorld(newParent), RangeError);
    checks++;
    check(child.parent === oldParent, 'non-finite rejection keeps the old parent');
    check(events === 0, 'non-finite rejection fires no events');
}

// ---------------------------------------------------------------------------
// 7b. Ordinary Float32 rounding must not be mistaken for shear.
// ---------------------------------------------------------------------------
for (const angles of [[12.3, -47.8, 33.1], [80.1, 5.5, -61.4], [-22.2, 111.7, 8.8]]) {
    const parentA = makeNode('parentA', new Vec3(2, -3, 1),
        new Quat().setFromEulerAngles(angles[0], angles[1], angles[2]), new Vec3(2, 2, 2));
    const parentB = makeNode('parentB', new Vec3(-4, 6, 0),
        new Quat().setFromEulerAngles(-angles[2], angles[0], angles[1]), new Vec3(0.5, 0.5, 0.5));
    const child = makeNode('child', new Vec3(1, -1, 2),
        new Quat().setFromEulerAngles(angles[1], angles[2], angles[0]), new Vec3(3, 3, 3));
    parentA.addChild(child);

    const worldBefore = snapshotWorld(child);
    child.reparentPreservingWorld(parentB);
    assertMat4Close(child.getWorldTransform(), worldBefore, `float32-noise reparent (${angles.join(',')})`);
}

// ---------------------------------------------------------------------------
// 7c. Detaching a sheared world matrix to the root is rejected just the same.
// ---------------------------------------------------------------------------
{
    const oldParent = makeNode('oldParent');
    const child = makeNode('child', new Vec3(1, 0, 0));
    oldParent.addChild(child);
    child.getWorldTransform();

    const sheared = new Mat4().copy(child.worldTransform);
    sheared.data[9] += 0.25;
    child.worldTransform.copy(sheared);

    let events = 0;
    child.on('remove', () => events++);
    const localPosBefore = child.getLocalPosition().clone();

    assert.throws(() => child.reparentPreservingWorld(null), RangeError);
    checks++;
    check(child.parent === oldParent, 'root shear rejection keeps the old parent');
    check(oldParent.children[0] === child, 'root shear rejection keeps child order');
    check(child.getLocalPosition().equals(localPosBefore), 'root shear rejection keeps local TRS');
    check(events === 0, 'root shear rejection fires no events');
}

// ---------------------------------------------------------------------------
// 8. A singular (non-invertible) new parent is rejected before removal.
// ---------------------------------------------------------------------------
{
    const oldParent = makeNode('oldParent');
    const newParent = makeNode('newParent', null, null, new Vec3(0, 1, 1));
    const child = makeNode('child', new Vec3(1, 0, 0));
    oldParent.addChild(child);

    let events = 0;
    child.on('remove', () => events++);
    newParent.on('childinsert', () => events++);

    assert.throws(() => child.reparentPreservingWorld(newParent), RangeError);
    checks++;
    check(child.parent === oldParent, 'singular parent rejection keeps the old parent');
    check(oldParent.children[0] === child, 'singular parent rejection keeps child order');
    check(events === 0, 'singular parent rejection fires no events');
}

// ---------------------------------------------------------------------------
// 9. The node itself or a descendant cannot be the new parent.
// ---------------------------------------------------------------------------
{
    const parent = makeNode('parent');
    const child = makeNode('child');
    const grandChild = makeNode('grandChild');
    parent.addChild(child);
    child.addChild(grandChild);

    assert.throws(() => child.reparentPreservingWorld(child), Error);
    checks++;
    assert.throws(() => child.reparentPreservingWorld(grandChild), Error);
    checks++;
    check(child.parent === parent, 'self-parent rejection keeps hierarchy');
    check(grandChild.parent === child, 'descendant-parent rejection keeps hierarchy');
}

// ---------------------------------------------------------------------------
// 10. Scale compensation anywhere relevant rejects before any hierarchy change.
// ---------------------------------------------------------------------------
function expectScaleCompensationRejection(node, target, label) {
    const oldParent = node.parent;
    const siblings = oldParent ? [...oldParent.children] : null;
    const localPos = node.getLocalPosition().clone();
    let events = 0;
    node.on('remove', () => events++);
    node.on('insert', () => events++);

    assert.throws(() => node.reparentPreservingWorld(target), RangeError, label);
    checks++;

    check(node.parent === oldParent, `${label}: parent unchanged`);
    if (oldParent) {
        check(oldParent.children.length === siblings.length &&
            oldParent.children.every((c, i) => c === siblings[i]), `${label}: child order unchanged`);
    }
    check(node.getLocalPosition().equals(localPos), `${label}: local TRS unchanged`);
    check(events === 0, `${label}: no events fired`);

    node.off('remove');
    node.off('insert');
}

{
    // On the moved node itself.
    const root = makeNode('root');
    const target = makeNode('target');
    const child = makeNode('child');
    root.addChild(child);
    child.scaleCompensation = true;
    expectScaleCompensationRejection(child, target, 'scale compensation on moved node');
    child.scaleCompensation = false;

    // On a descendant of the moved subtree.
    const grandChild = makeNode('grandChild');
    child.addChild(grandChild);
    grandChild.scaleCompensation = true;
    expectScaleCompensationRejection(child, target, 'scale compensation on descendant');
    grandChild.scaleCompensation = false;

    // On an old ancestor.
    root.scaleCompensation = true;
    expectScaleCompensationRejection(child, target, 'scale compensation on old ancestor');
    root.scaleCompensation = false;

    // On a new ancestor.
    const newGrand = makeNode('newGrand');
    newGrand.addChild(target);
    newGrand.scaleCompensation = true;
    expectScaleCompensationRejection(child, target, 'scale compensation on new ancestor');
    newGrand.scaleCompensation = false;
}

// ---------------------------------------------------------------------------
// 11. World matrices are already preserved when insert callbacks run.
// ---------------------------------------------------------------------------
{
    const parentA = makeNode('parentA', new Vec3(10, 0, 0), null, new Vec3(2, 2, 2));
    const parentB = makeNode('parentB', new Vec3(-4, 0, 0), null, new Vec3(3, 3, 3));
    const child = makeNode('child', new Vec3(1, 0, 0),
        new Quat().setFromEulerAngles(0, 30, 0));
    const grandChild = makeNode('grandChild', new Vec3(0, 1, 0));
    parentA.addChild(child);
    child.addChild(grandChild);

    const childWorldBefore = snapshotWorld(child);
    const grandWorldBefore = snapshotWorld(grandChild);

    child.on('insert', (newParent) => {
        check(newParent === parentB, 'insert event reports the new parent');
        assertMat4Close(child.getWorldTransform(), childWorldBefore, 'child world inside insert event');
    });
    grandChild.on('inserthierarchy', () => {
        assertMat4Close(grandChild.getWorldTransform(), grandWorldBefore, 'grandchild world inside inserthierarchy event');
    });
    parentB.on('childinsert', (inserted) => {
        check(inserted === child, 'childinsert event reports the child');
        assertMat4Close(inserted.getWorldTransform(), childWorldBefore, 'child world inside childinsert event');
    });

    child.reparentPreservingWorld(parentB);
}

// ---------------------------------------------------------------------------
// 12. Hierarchy events, enabled inheritance and graphDepth still behave.
// ---------------------------------------------------------------------------
{
    const rootA = makeNode('rootA');
    const rootB = makeNode('rootB');
    rootA._enabledInHierarchy = true;
    rootB._enabledInHierarchy = false;

    const child = makeNode('child');
    const grandChild = makeNode('grandChild');
    rootA.addChild(child);
    child.addChild(grandChild);

    const eventLog = [];
    child.on('remove', p => eventLog.push(`remove:${p.name}`));
    child.on('insert', p => eventLog.push(`insert:${p.name}`));
    rootA.on('childremove', c => eventLog.push(`childremove:${c.name}`));
    rootB.on('childinsert', c => eventLog.push(`inserted:${c.name}`));

    check(child.enabled === true, 'child enabled under enabled hierarchy');
    check(child.graphDepth === 1 && grandChild.graphDepth === 2, 'graph depth before reparent');

    child.reparentPreservingWorld(rootB);

    check(eventLog[0] === 'remove:rootA', 'remove event fired first');
    check(eventLog[1] === 'childremove:child', 'childremove event fired');
    check(eventLog[2] === 'insert:rootB', 'insert event fired');
    check(eventLog[3] === 'inserted:child', 'childinsert event fired');

    check(child.enabled === false, 'enabled inheritance follows the disabled new parent');
    check(child.graphDepth === 1 && grandChild.graphDepth === 2, 'graph depth recomputed after reparent');

    child.reparentPreservingWorld(rootA);
    check(child.enabled === true, 'enabled inheritance restored under the enabled parent');
}

// ---------------------------------------------------------------------------
// 13. Afterwards the normal dirty-flag machinery still drives updates.
// ---------------------------------------------------------------------------
{
    const parentA = makeNode('parentA', new Vec3(10, 0, 0), null, new Vec3(2, 2, 2));
    const parentB = makeNode('parentB', new Vec3(-4, 0, 0), null, new Vec3(3, 3, 3));
    const child = makeNode('child', new Vec3(1, 0, 0));
    const grandChild = makeNode('grandChild', new Vec3(0, 1, 0));
    parentA.addChild(child);
    child.addChild(grandChild);

    child.reparentPreservingWorld(parentB);
    assertClose(child.getPosition().x, 12, 'world x preserved before further edits');

    // A local edit applies relative to the new parent, through the existing dirty flags.
    child.setLocalPosition(16 / 3 + 2, 0, 0);
    assertClose(child.getPosition().x, 12 + 3 * 2, 'setLocalPosition moves through the new parent');

    // Moving the new parent carries the whole subtree along.
    const grandWorldBefore = snapshotWorld(grandChild);
    parentB.setLocalPosition(-4 + 5, 0, 0);
    const grandWorldAfter = grandChild.getWorldTransform();
    assertClose(grandWorldAfter.data[12] - grandWorldBefore.data[12], 5,
        'moving the new parent translates the subtree');
    assertClose(grandWorldAfter.data[13] - grandWorldBefore.data[13], 0,
        'subtree y follows the new parent');
}

// ---------------------------------------------------------------------------
// 14. Index insertion matches reparent's position rules.
// ---------------------------------------------------------------------------
{
    const parent = makeNode('parent');
    const a = makeNode('a');
    const b = makeNode('b');
    const c = makeNode('c');
    parent.addChild(a);
    parent.addChild(b);
    parent.addChild(c);

    const otherRoot = makeNode('otherRoot');
    const child = makeNode('child');
    otherRoot.addChild(child);
    const worldBefore = snapshotWorld(child);

    child.reparentPreservingWorld(parent, 1);
    check(parent.children[0] === a && parent.children[1] === child &&
          parent.children[2] === b && parent.children[3] === c, 'inserted at index 1');
    assertMat4Close(child.getWorldTransform(), worldBefore, 'indexed insert preserves world matrix');
    check(otherRoot.children.length === 0, 'indexed insert removed node from old parent');
}

assert.ok(checks > 0, 'the test file must execute real assertions');
console.log(`REPARENT_WORLD_OK (${checks} assertions)`);
