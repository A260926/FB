import { Mat4 } from '../src/core/math/mat4.js';
import { Quat } from '../src/core/math/quat.js';
import { Vec3 } from '../src/core/math/vec3.js';
import { GraphNode } from '../src/scene/graph-node.js';

const EPS = 1e-5;

// Combined relative + absolute tolerance used for world transform comparisons.
function approx(actual, expected) {
    return Math.abs(actual - expected) <= EPS * Math.max(1, Math.abs(actual), Math.abs(expected));
}

function matrixApprox(actual, expected) {
    for (let i = 0; i < 16; i++) {
        if (!approx(actual.data[i], expected.data[i])) {
            return false;
        }
    }
    return true;
}

// Independently multiplies the local transforms along the final parent chain,
// so world matrices are checked without trusting the engine cache under test.
function expectedWorld(node) {
    const chain = [];
    let current = node;
    while (current) {
        chain.unshift(current);
        current = current.parent;
    }
    const result = new Mat4();
    for (const n of chain) {
        result.mulAffine2(result, n.getLocalTransform());
    }
    return result;
}

function shapeOf(root) {
    const shape = [];
    root.forEach((node) => {
        shape.push({
            name: node.name,
            parent: node.parent ? node.parent.name : null,
            depth: node.graphDepth,
            children: node.children.map((child) => child.name)
        });
    });
    return shape;
}

function matrixSnapshot(root) {
    const snapshot = new Map();
    root.forEach((node) => {
        snapshot.set(node.name, {
            data: new Float32Array(node.worldTransform.data),
            dirtyWorld: node._dirtyWorld,
            frozen: node._frozen,
            enabledInHierarchy: node._enabledInHierarchy
        });
    });
    return snapshot;
}

function attachEventLog(nodes, log) {
    for (const node of nodes) {
        for (const event of ['remove', 'removehierarchy', 'insert', 'inserthierarchy', 'childremove', 'childinsert']) {
            node.on(event, (parent) => log.push(`${event}:${node.name}:${parent ? parent.name : 'null'}`));
        }
    }
}

// Minimal GraphNode derivative recording the hierarchy activation lifecycle.
class HookNode extends GraphNode {
    constructor(name) {
        super(name);
        this.stateLog = [];
        this.postLog = [];
        // Optional observer invoked after each state change, once the flag is final.
        this.stateObserver = null;
    }

    _onHierarchyStateChanged(enabled) {
        super._onHierarchyStateChanged(enabled);
        this.stateLog.push(enabled);
        if (this.stateObserver) {
            this.stateObserver(this, enabled);
        }
    }

    _onHierarchyStatePostChanged() {
        this.postLog.push(true);
    }
}

function buildTree() {
    const root = new GraphNode('root');
    const a = new GraphNode('a');
    const b = new GraphNode('b');
    const c = new GraphNode('c');
    const d = new GraphNode('d');
    const e = new GraphNode('e');
    root.addChild(a);
    root.addChild(b);
    a.addChild(c);
    c.addChild(d);
    b.addChild(e);
    return { root, a, b, c, d, e };
}

export const cases = {
    // Final tree shape, ordering, local transform preservation and frozen caches.
    finalGraph(check) {
        const { root, a, b, c, d, e } = buildTree();
        a.setLocalPosition(10, 0, 0);
        a.setLocalScale(2, 2, 2);
        c.setLocalPosition(1, 0, 0);
        c.setLocalRotation(new Quat().setFromEulerAngles(0, 90, 0));
        d.setLocalPosition(0, 2, 0);
        b.setLocalPosition(-4, 0, 0);
        e.setLocalPosition(3, 0, 0);

        // Grandparent and grandchild swap parents in one batch: c moves to root
        // while its old parent a moves underneath it.
        const returned = root.reparentBatch([
            { node: c, parent: root },
            { node: a, parent: c }
        ]);

        check(returned === root, 'reparentBatch returns the boundary root');
        check(c.parent === root, 'c ends up directly under root');
        check(a.parent === c, 'a ends up under its former descendant c');
        check(d.parent === c, 'd keeps its parent c');
        check(b.parent === root, 'unmoved sibling b stays under root');

        // Unmoved siblings keep relative order; moved nodes are appended in input order.
        check(JSON.stringify(root.children.map((n) => n.name)) === JSON.stringify(['b', 'c']),
            'root children keep b first and append c');
        check(JSON.stringify(c.children.map((n) => n.name)) === JSON.stringify(['d', 'a']),
            'c keeps d first and appends a');

        check(a.graphDepth === 2, 'a depth follows the final chain root-c-a');
        check(c.graphDepth === 1, 'c depth is 1');
        check(d.graphDepth === 2, 'd depth is 2');
        check(b.graphDepth === 1 && e.graphDepth === 2, 'unmoved branch depths unchanged');

        // Local TRS is preserved exactly.
        check(a.getLocalPosition().equals(new Vec3(10, 0, 0)), 'a local position preserved');
        check(a.getLocalScale().equals(new Vec3(2, 2, 2)), 'a local scale preserved');
        check(c.getLocalRotation().equals(new Quat().setFromEulerAngles(0, 90, 0)),
            'c local rotation preserved');
        check(d.getLocalPosition().equals(new Vec3(0, 2, 0)), 'd local position preserved');

        // World transforms follow the final parent chain within the 1e-5 tolerance.
        check(matrixApprox(d.getWorldTransform(), expectedWorld(d)), 'd world transform matches final chain');
        check(matrixApprox(a.getWorldTransform(), expectedWorld(a)), 'a world transform matches final chain');
        check(approx(d.getPosition().x, 1) && approx(d.getPosition().y, 2),
            'd world position is computed from the final root-c-d chain');

        // Append order with two nodes landing on the same new parent, plus a no-op record.
        const tree2 = buildTree();
        const before = shapeOf(tree2.root);
        tree2.root.reparentBatch([
            { node: tree2.d, parent: tree2.b },
            { node: tree2.c, parent: tree2.b },
            { node: tree2.e, parent: tree2.b } // final parent unchanged: no-op
        ]);
        check(JSON.stringify(tree2.b.children.map((n) => n.name)) === JSON.stringify(['e', 'd', 'c']),
            'moved nodes append in changes order after the unmoved sibling');
        check(tree2.e.parent === tree2.b, 'no-op record leaves the node in place');
        check(tree2.d.graphDepth === 2 && tree2.c.graphDepth === 2, 'moved depths recomputed');
        check(shapeOf(tree2.root).length === before.length, 'no node is lost or duplicated by the batch');

        // An empty batch is a no-op without touching anything.
        const shapeBeforeEmpty = shapeOf(tree2.root);
        const emptyReturn = tree2.root.reparentBatch([]);
        check(emptyReturn === tree2.root, 'empty batch returns the root');
        check(JSON.stringify(shapeOf(tree2.root)) === JSON.stringify(shapeBeforeEmpty),
            'empty batch leaves the tree intact');

        // Frozen hierarchy: getWorldTransform() inside a notification must resolve
        // against the final chain even though syncHierarchy froze the old tree.
        const tree3 = buildTree();
        tree3.a.setLocalPosition(10, 0, 0);
        tree3.c.setLocalPosition(1, 0, 0);
        tree3.d.setLocalPosition(0, 2, 0);
        tree3.b.setLocalPosition(-4, 0, 0);
        tree3.root.syncHierarchy();
        check(tree3.a._frozen && tree3.c._frozen && tree3.d._frozen, 'hierarchy is frozen before the batch');

        let insideEvent = null;
        // d is not moved itself, so it receives the hierarchy event as a descendant.
        tree3.d.on('inserthierarchy', () => {
            insideEvent = {
                parent: tree3.d.parent,
                position: tree3.d.getPosition().clone(),
                matrix: tree3.d.getWorldTransform().clone()
            };
        });
        tree3.root.reparentBatch([{ node: tree3.c, parent: tree3.b }]);

        check(insideEvent !== null, 'inserthierarchy notification fires after commit');
        check(insideEvent.parent === tree3.c, 'event handler sees the committed parent of the descendant');
        check(matrixApprox(insideEvent.matrix, expectedWorld(tree3.d)),
            'world matrix read in the hook matches the final chain');
        check(approx(insideEvent.position.x, -3) && approx(insideEvent.position.y, 2),
            'world position read in the hook is -3,2 from the final chain');
        check(!tree3.c._frozen && !tree3.d._frozen, 'moved subtree is unfrozen after the batch');
        check(matrixApprox(tree3.d.getWorldTransform(), expectedWorld(tree3.d)),
            'later world transform reads stay consistent with the final chain');
    },

    // Notification order, event arguments, final-state visibility and reentrancy.
    observers(check) {
        const tree = buildTree();
        const all = [tree.root, tree.a, tree.b, tree.c, tree.d, tree.e];
        const log = [];
        attachEventLog(all, log);

        tree.root.reparentBatch([
            { node: tree.d, parent: tree.b },
            { node: tree.a, parent: tree.b }
        ]);

        check(JSON.stringify(log) === JSON.stringify([
            // Record 1: d moves c -> b (leaf, no hierarchy events of its own).
            'remove:d:c',
            'childremove:c:d',
            'insert:d:b',
            'childinsert:b:d',
            // Record 2: a moves root -> b. Its batch-start subtree still contains
            // c and d; its final subtree only contains c.
            'remove:a:root',
            'removehierarchy:c:root',
            'removehierarchy:d:root',
            'childremove:root:a',
            'insert:a:b',
            'inserthierarchy:c:b',
            'childinsert:b:a'
        ]), 'the six notification phases fire per record in the specified order');

        // Nested move: d is notified both as a moved node and as a former descendant of a.
        const dEvents = log.filter((line) => line.endsWith(':d') || line.includes(':d:'));
        check(dEvents.some((line) => line.startsWith('remove:d:')) &&
              dEvents.some((line) => line.startsWith('removehierarchy:d:')),
            'a nested descendant receives notifications from different move roots');

        // Event arguments preserve old/new parent identities and handlers see the final tree.
        const tree2 = buildTree();
        let removeArg = null;
        let insertArg = null;
        let stateInsideEvent = null;
        tree2.c.on('remove', (oldParent) => {
            removeArg = oldParent;
            // Even the removal phase already reports the committed tree.
            stateInsideEvent = stateInsideEvent || {};
            stateInsideEvent.oldParentChildren = oldParent.children.map((n) => n.name);
        });
        tree2.c.on('insert', (newParent) => {
            insertArg = newParent;
            stateInsideEvent = {
                ...stateInsideEvent,
                parent: tree2.c.parent,
                siblings: newParent.children.map((n) => n.name),
                world: tree2.c.getWorldTransform().clone()
            };
        });
        tree2.root.reparentBatch([{ node: tree2.c, parent: tree2.b }]);

        check(removeArg === tree2.a, 'remove event keeps the old parent identity');
        check(insertArg === tree2.b, 'insert event keeps the new parent identity');
        check(JSON.stringify(stateInsideEvent.oldParentChildren) === JSON.stringify([]),
            'the old parent child list read in the remove event is already the committed one');
        check(stateInsideEvent.parent === tree2.b, 'parent read inside the event is the committed one');
        check(JSON.stringify(stateInsideEvent.siblings) === JSON.stringify(['e', 'c']),
            'children read inside the event show the committed list');
        check(matrixApprox(stateInsideEvent.world, expectedWorld(tree2.c)),
            'world transform read inside the event uses the final parent chain');

        // Empty and all-no-op batches fire nothing at all.
        const tree3 = buildTree();
        const quietLog = [];
        attachEventLog([tree3.root, tree3.a, tree3.b, tree3.c, tree3.d, tree3.e], quietLog);
        tree3.root.reparentBatch([]);
        tree3.root.reparentBatch([
            { node: tree3.a, parent: tree3.root },
            { node: tree3.e, parent: tree3.b }
        ]);
        check(quietLog.length === 0, 'empty and all-no-op batches produce no events');

        // Re-entrant batch during notification throws before changing anything.
        const tree4 = buildTree();
        let reentryError = null;
        tree4.a.on('insert', () => {
            try {
                tree4.root.reparentBatch([{ node: tree4.e, parent: tree4.a }]);
            } catch (err) {
                reentryError = err;
            }
        });
        tree4.root.reparentBatch([{ node: tree4.a, parent: tree4.b }]);
        check(reentryError instanceof Error, 're-entrant batch throws an Error');
        check(tree4.e.parent === tree4.b, 're-entrant batch applies no changes');
        check(tree4.a.parent === tree4.b, 'the outer batch still commits');

        // The lock is released once notifications finish.
        tree4.root.reparentBatch([{ node: tree4.e, parent: tree4.root }]);
        check(tree4.e.parent === tree4.root, 'another batch works after the notified batch completes');
    },

    // Effective enabled propagation, hook ordering and detached-root isolation.
    activation(check) {
        const root = new HookNode('root');
        const on = new HookNode('on');
        const off = new HookNode('off');
        const item = new HookNode('item');
        const tip = new HookNode('tip');
        const quiet = new HookNode('quiet');
        root.addChild(on);
        root.addChild(off);
        on.addChild(item);
        item.addChild(tip);
        root.addChild(quiet);
        off.enabled = false;

        // Simulate an attached, enabled root the same way the production baseline does.
        root._onHierarchyStateChanged(true);
        root._notifyHierarchyStateChanged(root, true);
        for (const node of [root, on, off, item, tip, quiet]) {
            node.stateLog.length = 0;
            node.postLog.length = 0;
        }

        // Move the enabled on/item/tip subtree under the disabled off node.
        root.reparentBatch([{ node: on, parent: off }]);

        check(on.enabled === false && item.enabled === false && tip.enabled === false,
            'the moved subtree becomes effectively disabled');
        check(off.enabled === false, 'the disabled parent stays disabled');
        check(quiet.enabled === true && quiet.stateLog.length === 0,
            'unrelated nodes keep their state and receive no activation hook');

        // Every effectively-changed node gets exactly one state hook, final-tree pre-order.
        check(JSON.stringify(on.stateLog) === JSON.stringify([false]), 'on receives one disable hook');
        check(JSON.stringify(item.stateLog) === JSON.stringify([false]), 'item receives one disable hook');
        check(JSON.stringify(tip.stateLog) === JSON.stringify([false]), 'tip receives one disable hook even though it was not moved');
        check(off.stateLog.length === 0 && quiet.stateLog.length === 0, 'unchanged nodes are not re-notified');

        // Post hooks run on the same nodes only after every state hook has run.
        check(on.postLog.length === 1 && item.postLog.length === 1 && tip.postLog.length === 1,
            'each changed node receives one post hook');
        check(off.postLog.length === 0 && quiet.postLog.length === 0, 'unchanged nodes receive no post hook');

        // The first state hook already observes every other node's final effective state.
        let seenFinalState = null;
        on.stateLog.length = 0;
        on.stateObserver = () => {
            seenFinalState = { item: item.enabled, tip: tip.enabled, on: on.enabled };
        };
        root.reparentBatch([{ node: on, parent: root }]);
        on.stateObserver = null;
        check(seenFinalState !== null && seenFinalState.item === true &&
              seenFinalState.tip === true && seenFinalState.on === true,
            'the first activation hook reads the final enabled state of the whole subtree');
        check(JSON.stringify(item.stateLog) === JSON.stringify([false, true]),
            'moving back under an enabled parent reactivates the descendants once each');
        check(JSON.stringify(tip.stateLog) === JSON.stringify([false, true]),
            'unmoved descendants reactivate through their moved ancestor');

        // A detached root keeps its outer context: nothing is auto-enabled by a batch.
        const detached = new HookNode('detached');
        const child = new HookNode('child');
        const leaf = new HookNode('leaf');
        detached.addChild(child);
        child.addChild(leaf);
        const target = new HookNode('target');
        detached.addChild(target);
        detached.reparentBatch([{ node: child, parent: target }]);
        check(detached.enabled === false, 'the detached root is not auto-enabled');
        check(child.enabled === false && leaf.enabled === false,
            'descendants of a detached root stay effectively disabled');
        check(child.stateLog.length === 0 && leaf.stateLog.length === 0,
            'no activation hooks fire while the root is detached');
        check(child.graphDepth === 2 && leaf.graphDepth === 3, 'depths still update on a detached root');
    },

    // Validation failures, atomicity, exception propagation and reuse afterwards.
    failure(check) {
        const tree = buildTree();
        const all = [tree.root, tree.a, tree.b, tree.c, tree.d, tree.e];
        const eventLog = [];
        attachEventLog(all, eventLog);

        const expectFailure = (changes, label) => {
            const shapeBefore = shapeOf(tree.root);
            const matricesBefore = matrixSnapshot(tree.root);
            let threw = false;
            try {
                tree.root.reparentBatch(changes);
            } catch (err) {
                threw = err instanceof Error;
            }
            check(threw, `${label} throws an Error`);
            check(JSON.stringify(shapeOf(tree.root)) === JSON.stringify(shapeBefore),
                `${label} leaves the tree shape unchanged`);
            const matricesAfter = matrixSnapshot(tree.root);
            let cachesMatch = true;
            for (const [name, before] of matricesBefore) {
                const node = tree.root.findByName(name);
                const after = matricesAfter.get(name);
                if (node._enabledInHierarchy !== before.enabledInHierarchy ||
                    node._dirtyWorld !== before.dirtyWorld || node._frozen !== before.frozen ||
                    !after.data.every((value, i) => value === before.data[i])) {
                    cachesMatch = false;
                }
            }
            check(cachesMatch, `${label} leaves caches and enabled state untouched`);
        };

        const foreign = new GraphNode('foreign');
        const foreignParent = new GraphNode('foreignParent');

        expectFailure(null, 'null changes');
        expectFailure(undefined, 'undefined changes');
        expectFailure({}, 'non-array changes');
        expectFailure('nope', 'string changes');
        expectFailure([null], 'a null record');
        expectFailure([5], 'a primitive record');
        expectFailure([{}], 'a record without node and parent');
        expectFailure([{ node: tree.a }], 'a record without parent');
        expectFailure([{ node: tree.a, parent: {} }], 'a record with a non-GraphNode parent');
        expectFailure([{ node: {}, parent: tree.root }], 'a record with a non-GraphNode node');
        expectFailure([{ node: tree.root, parent: tree.b }], 'moving the boundary root');
        expectFailure([{ node: tree.a, parent: tree.a }], 'parenting a node to itself');
        expectFailure([
            { node: tree.a, parent: tree.b },
            { node: tree.a, parent: tree.root }
        ], 'a duplicate node');
        expectFailure([{ node: foreign, parent: tree.root }], 'a node outside the boundary');
        expectFailure([{ node: tree.a, parent: foreignParent }], 'a parent outside the boundary');
        expectFailure([{ node: tree.a, parent: tree.c }], 'a single-record cycle (ancestor under descendant)');
        expectFailure([
            { node: tree.a, parent: tree.c },
            { node: tree.c, parent: tree.d }
        ], 'a multi-record cycle through moved nodes');

        check(eventLog.length === 0, 'failed batches fire no notifications');
        check(tree.root._reparentBatchLocked === false, 'no lock remains after validation failures');

        // A valid batch still works after all the rejected ones.
        tree.root.reparentBatch([{ node: tree.e, parent: tree.a }]);
        check(tree.e.parent === tree.a, 'the tree accepts batches after failures');

        // A throwing notification callback propagates, keeps the commit and releases the lock.
        const tree2 = buildTree();
        const boom = new Error('boom from notification');
        let received = null;
        tree2.c.on('insert', () => {
            throw boom;
        });
        try {
            tree2.root.reparentBatch([{ node: tree2.c, parent: tree2.b }]);
        } catch (err) {
            received = err;
        }
        check(received === boom, 'notification exceptions propagate to the caller');
        check(tree2.c.parent === tree2.b, 'the committed structure is not rolled back');
        check(tree2.root._reparentBatchLocked === false, 'the lock is released after a callback exception');

        // The same root is immediately reusable for another batch.
        const reuseLog = [];
        tree2.d.on('insert', (parent) => reuseLog.push(parent));
        tree2.root.reparentBatch([{ node: tree2.d, parent: tree2.b }]);
        check(tree2.d.parent === tree2.b, 'a batch succeeds after a previous callback exception');
        check(reuseLog.length === 1 && reuseLog[0] === tree2.b, 'the reuse batch notifies normally');

        // A throwing activation hook also releases the lock and keeps the committed tree.
        const tree3 = buildTree();
        tree3.root._onHierarchyStateChanged(true);
        tree3.root._notifyHierarchyStateChanged(tree3.root, true);
        tree3.b.enabled = false; // deactivates only b's own subtree (e)
        const hookBoom = new Error('boom from activation hook');
        tree3.c._onHierarchyStateChanged = (enabled) => {
            GraphNode.prototype._onHierarchyStateChanged.call(tree3.c, enabled);
            throw hookBoom;
        };
        let hookError = null;
        try {
            tree3.root.reparentBatch([{ node: tree3.a, parent: tree3.b }]);
        } catch (err) {
            hookError = err;
        }
        check(hookError === hookBoom, 'activation hook exceptions propagate to the caller');
        check(tree3.a.parent === tree3.b, 'the tree stays committed when an activation hook throws');
        check(tree3.root._reparentBatchLocked === false, 'the lock is released after a hook exception');
        tree3.root.reparentBatch([{ node: tree3.e, parent: tree3.root }]);
        check(tree3.e.parent === tree3.root, 'the root is reusable after an activation hook exception');
    }
};
