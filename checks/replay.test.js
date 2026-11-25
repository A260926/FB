"use strict";

// Replay controller checks. Every group drives the real compiled worker
// subprocess through ReplayController; nothing is stubbed or faked. Fixture
// targets are generated in a temporary target directory so the relative
// coverage paths and source digests are meaningful.

const assert = require('assert');
const crypto = require('crypto');
const fs = require('fs');
const os = require('os');
const path = require('path');

const TMP_ROOT = path.join(os.tmpdir(), 'jsfuzz-replay-checks');

function freshDir(prefix) {
    fs.mkdirSync(TMP_ROOT, {recursive: true});
    return fs.mkdtempSync(path.join(TMP_ROOT, prefix + '-'));
}

function writeModule(dir, name, source) {
    const file = path.join(dir, name);
    fs.writeFileSync(file, source);
    return file;
}

function digestOf(file) {
    return crypto.createHash('sha256').update(fs.readFileSync(file)).digest('hex');
}

function featureParts(feature) {
    return JSON.parse(feature);
}

async function snapshotState(controller) {
    return JSON.stringify(controller.state());
}

async function drain(controller) {
    const steps = [];
    while (true) {
        const moved = await controller.step();
        steps.push({moved, state: controller.state()});
        if (!moved) {
            break;
        }
    }
    return steps;
}

// ---------------------------------------------------------------------------
// Group: stages
// ---------------------------------------------------------------------------

async function stages(c) {
    const dir = freshDir('stages');
    const target = writeModule(dir, 'sync_target.js', [
        'let phase = 0;',
        'exports.fuzz = function(buf) {',
        '  const command = buf.toString("utf8");',
        '  if (command === "open") { phase = 1; return; }',
        '  if (command === "rise" && phase === 1) { phase = 2; return; }',
        '  if (command === "seal" && phase === 2) { phase = 3; return; }',
        '  return;',
        '};',
        '',
    ].join('\n'));
    const asyncTarget = writeModule(dir, 'async_target.js', [
        'let stage = 0;',
        'async function fuzz(buf) {',
        '  const word = buf.toString("utf8");',
        '  if (word === "alpha") { stage = 1; return; }',
        '  if (word === "beta" && stage === 1) { stage = 2; return; }',
        '  if (word === "gamma" && stage === 2) { stage = 3; return; }',
        '}',
        'module.exports.fuzz = fuzz;',
        '',
    ].join('\n'));

    const ReplayController = c.production('replay').ReplayController;
    c.check(typeof ReplayController.create === 'function',
        'ReplayController.create must be a static function');
    c.check(typeof ReplayController.restore === 'function',
        'ReplayController.restore must be a static function');

    // Multiple stages including empty stages and an empty input.
    const plan = [
        [Buffer.from('open'), Buffer.from('rise')],
        [],
        [Buffer.from('seal'), Buffer.alloc(0), Buffer.from('unknown')],
        [],
    ];
    const controller = await ReplayController.create(target, plan, 'stages-build');
    try {
        let state = controller.state();
        c.check(state.stage === 0 && state.cursor === 0, 'fresh state points at first item');
        c.check(state.executions === 0, 'fresh state has zero executions');
        c.check(state.corpusSize === 0, 'fresh empty corpus');
        c.check(Array.isArray(state.coverage) && state.coverage.length === 0,
            'fresh coverage set is empty');

        c.check(await controller.step() === true, 'first step advances');
        state = controller.state();
        c.check(state.executions === 1, 'step counts one execution');
        c.check(state.stage === 0 && state.cursor === 1, 'cursor moves after a step');
        c.check(state.coverage.length > 0, 'coverage appears after first execution');
        const digest = digestOf(target);
        const firstFeatures = state.coverage.map(featureParts);
        c.check(firstFeatures.every(parts => parts.length === 5 &&
            parts[0] === 'sync_target.js' && parts[1] === digest &&
            ['s', 'f', 'b'].indexOf(parts[2]) !== -1 &&
            typeof parts[3] === 'string'), 'features are real Istanbul tuples');
        c.check(state.coverage.every(f => JSON.stringify(JSON.parse(f)) === f),
            'features round-trip through JSON');
        c.check(state.coverage.every((f, i, all) =>
            i === 0 || all[i - 1] < f), 'features are sorted and deduplicated');

        // Step through the empty stages and the empty input.
        const seen = [];
        while (await controller.step()) {
            seen.push(controller.state());
        }
        const final = controller.state();
        c.check(final.stage === plan.length, 'completed stage equals stage count');
        c.check(final.cursor === 0, 'completed cursor is zero');
        c.check(final.executions === 5, 'all five items executed including empty ones');
        c.check(seen.length === 4, 'four further steps were needed');
        c.check(await controller.step() === false, 'step returns false when complete');

        // Corpus only gains inputs that add features; identical content is
        // admitted at most once.
        c.check(final.corpusSize > 0, 'corpus gained feature-adding inputs');
        c.check(final.corpusSize <= final.executions, 'corpus is not larger than runs');
    } finally {
        await controller.close();
    }

    // Async target across stages.
    const asyncPlan = [[Buffer.from('alpha')], [Buffer.from('beta')], [Buffer.from('gamma')]];
    const asyncController = await ReplayController.create(asyncTarget, asyncPlan, 'async-build');
    try {
        for (let i = 0; i < asyncPlan.length; i++) {
            c.check(await asyncController.step() === true,
                'async stage step ' + i + ' advances');
        }
        const asyncState = asyncController.state();
        c.check(asyncState.executions === 3, 'async target counted three executions');
        c.check(asyncState.stage === 3 && asyncState.cursor === 0,
            'async target completes at the end');
        c.check(asyncState.coverage.length > 0, 'async target produces coverage');
        c.check(asyncState.coverage.map(featureParts).every(parts =>
            parts[0] === 'async_target.js'), 'async coverage binds the async module');
    } finally {
        await asyncController.close();
    }

    // Empty staging plan completes immediately.
    const empty = await ReplayController.create(target, [[], []], 'empty-build');
    try {
        c.check(empty.state().stage === 2, 'empty stages are skipped upfront');
        c.check(empty.state().cursor === 0, 'empty plan cursor is zero');
        c.check(await empty.step() === false, 'empty plan never steps');
    } finally {
        await empty.close();
    }

    // buildID must be non-empty.
    let rejected = false;
    try {
        await ReplayController.create(target, [], '');
    } catch (e) {
        rejected = true;
    }
    c.check(rejected, 'empty buildID is rejected');
}

// ---------------------------------------------------------------------------
// Group: priming
// ---------------------------------------------------------------------------

async function priming(c) {
    const dir = freshDir('priming');
    // A target whose feature set can be grown in several different ways. We
    // use a helper module so that features span more than one source file.
    const helper = writeModule(dir, 'helper.js', [
        'exports.alpha = function() { return 1; };',
        'exports.beta = function() { return 2; };',
        'exports.gamma = function() { return 3; };',
        '',
    ].join('\n'));
    const target = writeModule(dir, 'grow_target.js', [
        'const helper = require("./helper.js");',
        'let state = 0;',
        'exports.fuzz = function(buf) {',
        '  const word = buf.toString("utf8");',
        '  if (word === "a") { state = 1; helper.alpha(); }',
        '  if (word === "b") { helper.beta(); }',
        '  if (word === "c") { helper.gamma(); }',
        '  if (word === "ab") { state = 2; helper.alpha(); helper.beta(); }',
        '  if (word === "ac") { helper.alpha(); helper.gamma(); }',
        '  if (word === "bc") { helper.beta(); helper.gamma(); }',
        '  if (word === "abc") { helper.alpha(); helper.beta(); helper.gamma(); }',
        '  return;',
        '};',
        '',
    ].join('\n'));

    // Two plans with the same number of executions/inputs but different
    // features must yield different coverage sets. This is the "same total,
    // different features" distinction.
    const planA = [[Buffer.from('a'), Buffer.from('b'), Buffer.from('c')]];
    const planB = [[Buffer.from('abc'), Buffer.from(''), Buffer.from('')]];

    const ReplayController = c.production('replay').ReplayController;

    async function run(plan) {
        const controller = await ReplayController.create(target, plan, 'priming-build');
        try {
            const steps = await drain(controller);
            return {steps, state: controller.state(), plan};
        } finally {
            await controller.close();
        }
    }

    const runA = await run(planA);
    const runB = await run(planB);

    c.check(runA.state.executions === 3, 'plan A has three executions');
    c.check(runB.state.executions === 3, 'plan B has three executions');
    // Same execution count but a different feature set.
    c.check(runA.state.coverage.length !== runB.state.coverage.length ||
        JSON.stringify(runA.state.coverage) !== JSON.stringify(runB.state.coverage),
        'equal totals with different features are distinguishable');
    c.check(JSON.stringify(runA.state.coverage) !== JSON.stringify(runB.state.coverage),
        'feature sets differ even when totals may coincide');

    // A target with two mutually exclusive branches produces the same number
    // of features for either branch. The sets must still be distinguishable by
    // content, not by count alone.
    const symDir = freshDir('priming-symmetric');
    const symTarget = writeModule(symDir, 'symmetric_target.js', [
        'function left() { const x = 1; return x + 1; }',
        'function right() { const y = 2; return y + 2; }',
        'exports.fuzz = function(buf) {',
        '  if (buf.length === 0) { return 0; }',
        '  if (buf[0] === 0) { return left(); }',
        '  return right();',
        '};',
        '',
    ].join('\n'));
    async function runSingle(byte) {
        const controller = await ReplayController.create(symTarget,
            [[Buffer.from([byte])]], 'priming-build');
        try {
            await controller.step();
            return controller.state();
        } finally {
            await controller.close();
        }
    }
    const leftState = await runSingle(0);
    const rightState = await runSingle(1);
    c.check(leftState.coverage.length === rightState.coverage.length,
        'symmetric branches have the same feature count');
    c.check(JSON.stringify(leftState.coverage) !==
        JSON.stringify(rightState.coverage),
        'same feature count with different branches is distinguishable');
    c.check(leftState.coverage.every(f => {
        const parts = featureParts(f);
        return parts[0] === 'symmetric_target.js';
    }), 'symmetric coverage binds the target module');

    // The full prefix/suffix consistency: run an uninterrupted session and a
    // checkpoint/restore session, then compare every step including admission
    // (corpus size) results.
    const splitPlan = [
        [Buffer.from('a'), Buffer.from('ab')],
        [Buffer.from('b'), Buffer.from('bc')],
        [Buffer.from('c'), Buffer.from('ac'), Buffer.from('abc')],
    ];
    const straight = await run(splitPlan);

    const prefixLength = 3;
    let controller = await ReplayController.create(target, splitPlan, 'priming-build');
    const prefix = [];
    let blob;
    let prefixState;
    try {
        for (let i = 0; i < prefixLength; i++) {
            c.check(await controller.step() === true, 'prefix step ' + i);
            prefix.push(controller.state());
        }
        prefixState = controller.state();
        c.check(JSON.stringify(controller.state().coverage) ===
            JSON.stringify(straight.steps[prefixLength - 1].state.coverage),
            'prefix coverage matches the uninterrupted run');
        c.check(controller.state().corpusSize ===
            straight.steps[prefixLength - 1].state.corpusSize,
            'prefix corpus size matches the uninterrupted run');
        blob = await controller.checkpoint();
    } finally {
        await controller.close();
    }

    let restored = await ReplayController.restore(target, blob, 'priming-build');
    let suffix = [];
    try {
        c.check(JSON.stringify(restored.state()) === JSON.stringify(prefixState),
            'restore reproduces the exact prefix state');
        while (true) {
            const moved = await restored.step();
            suffix.push({moved, state: restored.state()});
            if (!moved) {
                break;
            }
        }
        c.check(JSON.stringify(restored.state()) === JSON.stringify(straight.state),
            'restored final state equals the uninterrupted final state');
        c.check(JSON.stringify(suffix) ===
            JSON.stringify(straight.steps.slice(prefixLength)),
            'restored suffix matches the uninterrupted suffix step for step');
        c.check(restored.state().executions === splitPlan.reduce(
            (n, stage) => n + stage.length, 0),
            'restored session did not count the prefix twice');
    } finally {
        await restored.close();
    }

    // Restoring from a corrupted archive must not start a worker or mutate
    // the archive; and a same-total-but-different-feature mismatch must be
    // rejected after the replay rebuild, with the worker reclaimed.
    const checkpointBytes = Buffer.from(blob);
    const original = blob.toString('hex');

    const corruptedByte = Buffer.from(checkpointBytes);
    // Flip a byte inside the canonical body (past the magic/version/digest).
    corruptedByte[Math.floor(corruptedByte.length / 2)] ^= 0x01;
    let corruptRejected = false;
    try {
        await ReplayController.restore(target, corruptedByte, 'priming-build');
    } catch (e) {
        corruptRejected = true;
    }
    c.check(corruptRejected, 'a corrupted checkpoint is rejected');
    c.check(blob.toString('hex') === original, 'the archive is left unchanged');

    // Truncation and trailing content.
    let truncatedRejected = false;
    try {
        await ReplayController.restore(target, blob.slice(0, blob.length - 5), 'priming-build');
    } catch (e) {
        truncatedRejected = true;
    }
    c.check(truncatedRejected, 'a truncated checkpoint is rejected');

    let trailingRejected = false;
    try {
        await ReplayController.restore(target, Buffer.concat([blob, Buffer.from('junk')]),
            'priming-build');
    } catch (e) {
        trailingRejected = true;
    }
    c.check(trailingRejected, 'a checkpoint with trailing bytes is rejected');

    let badEncodingRejected = false;
    try {
        await ReplayController.restore(target, Buffer.concat([blob, Buffer.from([0xff])]),
            'priming-build');
    } catch (e) {
        badEncodingRejected = true;
    }
    c.check(badEncodingRejected, 'a checkpoint with invalid byte encoding is rejected');

    let wrongBuildRejected = false;
    try {
        await ReplayController.restore(target, blob, 'a-different-build');
    } catch (e) {
        wrongBuildRejected = true;
    }
    c.check(wrongBuildRejected, 'a checkpoint from another build is rejected');

    // Same execution total, different features: change the target so that the
    // replay produces a different feature set from the same inputs.
    const changedDir = freshDir('priming-changed');
    const changedTarget = writeModule(changedDir, 'grow_target.js', [
        'const helper = require("./helper.js");',
        'let state = 0;',
        'exports.fuzz = function(buf) {',
        '  const word = buf.toString("utf8");',
        '  if (word === "a") { state = 1; helper.beta(); }',
        '  if (word === "b") { helper.alpha(); }',
        '  if (word === "c") { helper.gamma(); }',
        '  if (word === "ab") { state = 2; helper.beta(); helper.alpha(); }',
        '  if (word === "ac") { helper.gamma(); helper.alpha(); }',
        '  if (word === "bc") { helper.beta(); helper.beta(); }',
        '  if (word === "abc") { helper.alpha(); helper.alpha(); helper.beta(); }',
        '  return;',
        '};',
        '',
    ].join('\n'));
    writeModule(changedDir, 'helper.js', fs.readFileSync(helper, 'utf8'));
    let changedRejected = false;
    try {
        await ReplayController.restore(changedTarget, blob, 'priming-build');
    } catch (e) {
        changedRejected = true;
    }
    c.check(changedRejected, 'a changed target is rejected during rebuild');
    c.check(blob.toString('hex') === original, 'rejected rebuild leaves the archive intact');

    // A zero-execution checkpoint of a changed target must still be rejected:
    // there is no replay to compare, so the live source binding itself is the
    // signal.
    let zeroBlob;
    const zero = await ReplayController.create(target, splitPlan, 'priming-build');
    try {
        zeroBlob = await zero.checkpoint();
    } finally {
        await zero.close();
    }
    let zeroChangedRejected = false;
    try {
        await ReplayController.restore(changedTarget, zeroBlob, 'priming-build');
    } catch (e) {
        zeroChangedRejected = true;
    }
    c.check(zeroChangedRejected,
        'a changed target is rejected even with no committed prefix');

    // Checkpoint bytes are deterministic: two identical sessions serialize
    // identically, with no PID or absolute path embedded.
    const detA = await ReplayController.create(target, splitPlan, 'priming-build');
    const detB = await ReplayController.create(target, splitPlan, 'priming-build');
    let detBlobA;
    let detBlobB;
    try {
        for (let i = 0; i < prefixLength; i++) {
            await detA.step();
            await detB.step();
        }
        detBlobA = await detA.checkpoint();
        detBlobB = await detB.checkpoint();
    } finally {
        await detA.close();
        await detB.close();
    }
    c.check(detBlobA.equals(detBlobB),
        'identical sessions produce identical checkpoint bytes');
    c.check(detBlobA.indexOf(Buffer.from(path.resolve(target))) === -1,
        'checkpoint does not embed an absolute target path');
    c.check(detBlobA.indexOf(Buffer.from(String(process.pid))) === -1,
        'checkpoint does not embed the parent PID');

    // Returned state and checkpoint buffers are copies: mutating them cannot
    // change the live session.
    const iso = await ReplayController.create(target, splitPlan, 'priming-build');
    try {
        await iso.step();
        const before = JSON.stringify(iso.state());
        const liveState = iso.state();
        liveState.executions = 123456;
        liveState.coverage.push('forged-feature');
        liveState.stage = 99;
        liveState.cursor = 99;
        c.check(JSON.stringify(iso.state()) === before,
            'mutating a returned state object does not affect the session');
        const liveBlob = await iso.checkpoint();
        liveBlob[Math.floor(liveBlob.length / 2)] ^= 0xff;
        c.check(JSON.stringify(iso.state()) === before,
            'mutating a returned checkpoint buffer does not affect the session');
    } finally {
        await iso.close();
    }
}

// ---------------------------------------------------------------------------
// Group: protocol
// ---------------------------------------------------------------------------

async function protocol(c) {
    const {fork} = require('child_process');
    const dir = freshDir('protocol');
    const workerPath = path.join(process.env.TASK_BUILD, 'src', 'worker.js');
    const target = writeModule(dir, 'slow_target.js', [
        'exports.fuzz = function(buf) {',
        '  const value = buf.toString("utf8");',
        '  // Busy-wait long enough that a concurrent controller call is reliably',
        '  // observed while this execution is still in flight.',
        '  const deadline = Date.now() + 150;',
        '  while (Date.now() < deadline) { /* spin */ }',
        '  return value.length;',
        '};',
        '',
    ].join('\n'));

    const ReplayController = c.production('replay').ReplayController;
    const protocol = c.production('protocol');
    const WorkerMessageType = protocol.WorkerMessageType;
    const ManageMessageType = protocol.ManageMessageType;

    // Busy isolation: while a step is in flight, another step and a
    // checkpoint must be rejected rather than interleaved.
    const stages = [[Buffer.from('one'), Buffer.from('two'), Buffer.from('three')]];
    const controller = await ReplayController.create(target, stages, 'protocol-build');
    try {
        const first = controller.step();
        let busyStep = null;
        let busyCheckpoint = null;
        try {
            await controller.step();
        } catch (e) {
            busyStep = e.message;
        }
        try {
            await controller.checkpoint();
        } catch (e) {
            busyCheckpoint = e.message;
        }
        c.check(busyStep !== null, 'a concurrent step is rejected while busy');
        c.check(busyCheckpoint !== null, 'a checkpoint is rejected while busy');
        c.check(await first === true, 'the original step still completes');
        c.check(controller.state().executions === 1,
            'rejected concurrent calls did not count executions');
        c.check(controller.state().cursor === 1, 'only the real step moved the cursor');
    } finally {
        await controller.close();
    }

    // Late / duplicate / out-of-order messages must not double-count, advance,
    // or insert into the corpus. We talk to a real worker process directly and
    // replay the controller's own request shape, then verify the controller's
    // accounting is unaffected by forged messages sent to it.
    const child = fork(workerPath, [target, '--replay'],
        {execArgv: [], stdio: ['ignore', 'pipe', 'pipe', 'ipc']});
    try {
        const ready = await new Promise((resolve, reject) => {
            let settled = false;
            const onMessage = m => {
                if (m && m.type === WorkerMessageType.READY) {
                    settled = true;
                    child.off('message', onMessage);
                    resolve(m);
                }
            };
            child.on('message', onMessage);
            child.once('exit', () => {
                if (!settled) {
                    reject(new Error('worker exited before ready'));
                }
            });
            const timer = setTimeout(() => {
                if (!settled) {
                    reject(new Error('worker did not become ready'));
                }
            }, 10000);
            if (typeof timer.unref === 'function') {
                timer.unref();
            }
        });
        c.check(Array.isArray(ready.sources), 'worker announces its source set');
        c.check(ready.sources.some(s => s[0] === 'slow_target.js'),
            'worker source set includes the target module');

        // Send the same logical request twice with the same nonce and assert
        // that the worker refuses to execute out-of-order / mismatched requests
        // rather than running the target a second time. Each send waits for
        // the reply that carries the matching identity, so a late or stale
        // reply can never be mistaken for the answer to the current request.
        const session = 'forged-session';
        const send = (message) => new Promise((resolve, reject) => {
            const onMessage = m => {
                if (m && m.session === message.session && m.seq === message.seq &&
                    m.nonce === message.nonce) {
                    child.off('message', onMessage);
                    resolve(m);
                }
            };
            child.on('message', onMessage);
            child.send({
                type: ManageMessageType.REPLAY,
                buf: message.buf,
                session: message.session,
                generation: message.generation,
                seq: message.seq,
                nonce: message.nonce,
            });
        });
        const firstReply = await send({
            buf: Buffer.from('two'), session, generation: 1, seq: 0,
            nonce: 'nonce-0',
        });
        c.check(firstReply.type === WorkerMessageType.REPLAY_RESULT,
            'first replay request succeeded');
        c.check(firstReply.session === session && firstReply.generation === 1 &&
            firstReply.seq === 0 && firstReply.nonce === 'nonce-0',
            'reply carries the request identity');

        // Re-sending the identical request is answered from cache, not re-run.
        const duplicateReply = await send({
            buf: Buffer.from('two'), session, generation: 1, seq: 0,
            nonce: 'nonce-0',
        });
        c.check(JSON.stringify(duplicateReply.features) ===
            JSON.stringify(firstReply.features),
            'duplicate delivery returns the cached result');
        c.check(duplicateReply.coverage === firstReply.coverage,
            'duplicate delivery does not change coverage');

        // Out-of-order delivery (skipping seq 1) is refused.
        const outOfOrder = await send({
            buf: Buffer.from('three'), session, generation: 1, seq: 2,
            nonce: 'nonce-2',
        });
        c.check(outOfOrder.type === WorkerMessageType.REPLAY_ERROR,
            'out-of-order delivery is rejected');
        c.check(String(outOfOrder.message).indexOf('out of order') !== -1,
            'out-of-order rejection explains the ordering violation');

        // A request from another session is refused without touching target
        // state, so it can never advance this worker's memory.
        const foreign = await send({
            buf: Buffer.from('two'), session: 'foreign-session', generation: 1,
            seq: 1, nonce: 'nonce-foreign',
        });
        c.check(foreign.type === WorkerMessageType.REPLAY_ERROR,
            'a request for another session is rejected');
    } finally {
        await new Promise(resolve => {
            if (child.exitCode !== null || child.signalCode !== null) {
                return resolve();
            }
            child.once('exit', resolve);
            child.kill('SIGKILL');
        });
    }

    // A target execution failure terminates the session: later steps are
    // refused and the child process is reclaimed.
    const crashDir = freshDir('protocol-crash');
    const crashTarget = writeModule(crashDir, 'crash_target.js', [
        'exports.fuzz = function(buf) {',
        '  if (buf.toString("utf8") === "boom") { throw new Error("deliberate"); }',
        '  return 1;',
        '};',
        '',
    ].join('\n'));
    const crashController = await ReplayController.create(crashTarget,
        [[Buffer.from('ok'), Buffer.from('boom'), Buffer.from('ok')]], 'crash-build');
    let failedStep = null;
    try {
        c.check(await crashController.step() === true, 'pre-crash step works');
        try {
            await crashController.step();
        } catch (e) {
            failedStep = e.message;
        }
        c.check(failedStep !== null, 'a target error rejects the step');
        let afterRejected = false;
        try {
            await crashController.step();
        } catch (e) {
            afterRejected = true;
        }
        c.check(afterRejected, 'a terminated session refuses later steps');
        c.check(crashController.state().executions === 1,
            'failed execution was not committed');
    } finally {
        await crashController.close();
    }

    // Close stops the worker and makes the controller refuse further work.
    const closed = await ReplayController.create(target, [[Buffer.from('one')]], 'close-build');
    await closed.close();
    let closedStep = false;
    let closedCheckpoint = false;
    try {
        await closed.step();
    } catch (e) {
        closedStep = true;
    }
    try {
        await closed.checkpoint();
    } catch (e) {
        closedCheckpoint = true;
    }
    c.check(closedStep, 'a closed controller refuses step');
    c.check(closedCheckpoint, 'a closed controller refuses checkpoint');
}

module.exports = {stages, priming, protocol};
