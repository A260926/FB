#!/usr/bin/env node
"use strict";

// Demonstrates a bounded, stateful corpus replay that can be split across
// processes. The main process launches two independent parent processes:
//
//   * one runs the whole session in a single controller;
//   * one runs a prefix, checkpoints, closes, and restores into a fresh
//     controller before running the suffix.
//
// Their per-step state records are compared and the verdict is written to the
// supplied output file together with the records.

const fs = require('fs');
const os = require('os');
const path = require('path');
const assert = require('assert');
const cp = require('child_process');

const REPO_ROOT = path.resolve(__dirname, '..');
const TARGET = path.join(REPO_ROOT, 'task_support', 'benign_target.js');

function buildStages() {
    return [
        [Buffer.from('begin'), Buffer.from('advance')],
        [Buffer.from('finish'), Buffer.from('begin')],
        [],
        [Buffer.from('advance'), Buffer.from('finish'), Buffer.from('advance')],
        [Buffer.alloc(0), Buffer.from('begin')],
    ];
}

function snapshot(state) {
    return {
        stage: state.stage,
        cursor: state.cursor,
        executions: state.executions,
        corpusSize: state.corpusSize,
        coverage: state.coverage.slice(),
    };
}

async function drive(controller, log) {
    while (true) {
        const moved = await controller.step();
        const state = controller.state();
        log.push({moved, state: snapshot(state)});
        if (!moved) {
            break;
        }
    }
    return log;
}

async function runFull() {
    const {ReplayController} = require(path.join(process.env.TASK_BUILD, 'src', 'replay.js'));
    const controller = await ReplayController.create(TARGET, buildStages(), 'replay-example-build');
    try {
        return await drive(controller, []);
    } finally {
        await controller.close();
    }
}

async function runResume() {
    const {ReplayController} = require(path.join(process.env.TASK_BUILD, 'src', 'replay.js'));
    const stages = buildStages();
    const splitAfter = 5;
    const log = [];
    let controller = await ReplayController.create(TARGET, stages, 'replay-example-build');
    let checkpoint;
    let beforeSplit;
    try {
        for (let i = 0; i < splitAfter; i++) {
            const moved = await controller.step();
            const state = controller.state();
            log.push({moved, state: snapshot(state)});
        }
        beforeSplit = snapshot(controller.state());
        checkpoint = await controller.checkpoint();
    } finally {
        await controller.close();
    }

    controller = await ReplayController.restore(TARGET, checkpoint, 'replay-example-build');
    try {
        const restored = snapshot(controller.state());
        assert.deepStrictEqual(restored, beforeSplit,
            'restored state must match the checkpointed state');
        log.push({restored: restored});
        await drive(controller, log);
        return log;
    } finally {
        await controller.close();
    }
}

function runChild(mode, resultPath) {
    const run = mode === 'full' ? runFull : runResume;
    run().then(log => {
        fs.writeFileSync(resultPath, JSON.stringify(log));
        process.exit(0);
    }).catch(err => {
        console.error(err && err.stack || err);
        process.exit(1);
    });
}

function spawnChild(mode, resultPath) {
    const result = cp.spawnSync(process.execPath, [__filename, '--child', mode, resultPath], {
        stdio: 'inherit',
        env: process.env,
        timeout: 40000,
    });
    assert.strictEqual(result.status, 0, `example child "${mode}" failed`);
}

function main(outputPath) {
    if (!outputPath) {
        throw new Error('usage: examples/replay.js <output-file>');
    }
    const scratch = fs.mkdtempSync(
        path.join(path.dirname(path.resolve(outputPath)), 'replay-example-'));
    const fullPath = path.join(scratch, 'full.json');
    const resumePath = path.join(scratch, 'resume.json');
    spawnChild('full', fullPath);
    spawnChild('resume', resumePath);

    const fullLog = JSON.parse(fs.readFileSync(fullPath, 'utf8'));
    const resumeLog = JSON.parse(fs.readFileSync(resumePath, 'utf8'))
        .filter(entry => entry.restored === undefined);

    assert.deepStrictEqual(fullLog, resumeLog,
        'resumed suffix must match the uninterrupted session exactly');
    assert(fullLog.length > 1, 'expected at least one step in the example');

    const finalState = fullLog[fullLog.length - 1].state;
    assert.strictEqual(finalState.stage, buildStages().length,
        'final stage must equal the number of stages');
    assert.strictEqual(finalState.cursor, 0, 'final cursor must be zero');

    const lines = ['stateful_replay_ok', 'records=' + JSON.stringify(fullLog)];
    fs.writeFileSync(path.resolve(outputPath), lines.join('\n') + '\n');
    console.log('example wrote stateful_replay_ok');
}

if (process.argv[2] === '--child') {
    runChild(process.argv[3], process.argv[4]);
} else {
    try {
        main(process.argv[2]);
    } catch (err) {
        console.error(err && err.stack || err);
        process.exit(1);
    }
}
