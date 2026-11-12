// ============================================================================
// tests/run_coefficients.mjs
//
// Drives the offline MPEG-1 coefficient regression in tests/coefficients.c.
//
//   1. Compiles the test against the untouched pl_mpeg.h and runs every
//      numerical assertion.  A clean run prints:
//          MPEG_COEFF_OK cases=<n>      (n must be non-zero)
//
//   2. In separate temporary directories it copies pl_mpeg.h and produces
//      three independently compilable, deliberately broken variants:
//          MATRIX_MAP_CAUGHT   - custom quant matrices are stored without the
//                                 zigzag remap;
//          ODDIFY_CAUGHT       - the even->odd adjustment after
//                                 dequantization is disabled;
//          WORKSPACE_CLEAR_CAUGHT - block_data is not zeroed after the full
//                                 IDCT path, so transform workspace leaks into
//                                 the next block.
//      Each variant is compiled with the SAME coefficients.c and the SAME
//      expected values (no tolerance is relaxed and no variant gets special
//      expectations).  A variant only "catches" the defect when the binary
//      compiles, runs a non-empty set of scenes, and exits 42 on a
//      COEFF_ASSERT.  Compile failure, crash (signal), empty run or timeout
//      are explicitly rejected as detections.
//
// Only Node built-in modules are used; nothing is installed and the real
// pl_mpeg.h is never modified.
// ============================================================================

import { spawnSync } from 'node:child_process';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const repoRoot = path.dirname(path.dirname(fileURLToPath(import.meta.url)));
const headerPath = path.join(repoRoot, 'pl_mpeg.h');
const testSrcPath = path.join(repoRoot, 'tests', 'coefficients.c');
const CC = process.env.CC || 'cc';

function fail(message) {
    console.error(`RUNNER_ERROR ${message}`);
    process.exit(1);
}

function run(cmd, args, opts = {}) {
    return spawnSync(cmd, args, { encoding: 'utf8', timeout: 60000, ...opts });
}

// Compile coefficients.c (placed in workDir) against the pl_mpeg.h in that
// same directory, producing `binary`.  Returns the spawn result.
function compile(workDir, binary) {
    const src = path.join(workDir, 'coefficients.c');
    return run(CC, ['-std=c99', '-O0', '-I.', src, '-lm', '-o', binary], {
        cwd: workDir,
    });
}

// --------------------------------------------------------------------------
// Step 1: clean run against the original header
// --------------------------------------------------------------------------

const originalHeader = fs.readFileSync(headerPath, 'utf8');

const cleanDir = fs.mkdtempSync(path.join(os.tmpdir(), 'plm-coeff-clean-'));
fs.copyFileSync(headerPath, path.join(cleanDir, 'pl_mpeg.h'));
fs.copyFileSync(testSrcPath, path.join(cleanDir, 'coefficients.c'));

const cleanBin = path.join(cleanDir, 'coefficients');
const cleanBuild = run(CC,
    ['-std=c99', '-O0', '-I.', 'coefficients.c', '-lm', '-o', 'coefficients'],
    { cwd: cleanDir });
if (cleanBuild.status !== 0) {
    fail(`baseline compile failed:\n${cleanBuild.stderr}`);
}

const cleanRun = run(cleanBin, [], { cwd: cleanDir });
process.stdout.write(cleanRun.stdout);
if (cleanRun.status !== 0) {
    process.stderr.write(cleanRun.stderr);
    fail(`baseline numerical run exited ${cleanRun.status}`);
}

const okMatch = cleanRun.stdout.match(/^MPEG_COEFF_OK cases=([1-9][0-9]*)\b/m);
if (!okMatch) {
    fail('baseline did not print a non-zero MPEG_COEFF_OK cases=<n> line');
}
const caseCount = okMatch[1];

// Each mutation: where it lives in pl_mpeg.h, the exact source text, how to
// rewrite it, and the marker printed when the resulting defect is caught.
const mutations = [
    {
        marker: 'MATRIX_MAP_CAUGHT',
        location: 'plm_video_decode_sequence_header() ' +
                  '(intra and non-intra quant matrix loaders)',
        description: 'load custom quant matrices without the zigzag remap',
        // Two occurrences (intra and non-intra matrix loaders).
        search: 'int idx = PLM_VIDEO_ZIG_ZAG[i];',
        replace: 'int idx = i; /* MUTATION: zigzag remap removed */',
        expectedOccurrences: 2,
    },
    {
        marker: 'ODDIFY_CAUGHT',
        location: 'plm_video_decode_block() dequantize/oddify/clip section',
        description: 'disable even->odd adjustment after dequantization',
        search:
            '\t\tif ((level & 1) == 0) {\n' +
            '\t\t\tlevel -= level > 0 ? 1 : -1;\n' +
            '\t\t}',
        replace: '\t\t/* MUTATION: even->odd adjustment removed */',
        expectedOccurrences: 1,
    },
    {
        marker: 'WORKSPACE_CLEAR_CAUGHT',
        location: 'plm_video_decode_block() after plm_video_idct() ' +
                  '(intra overwrite and inter add paths)',
        description: 'do not clear block_data after the full IDCT path',
        // Two occurrences (intra overwrite path and inter add path).
        search: 'memset(self->block_data, 0, sizeof(self->block_data));',
        replace: '/* MUTATION: block_data workspace clear removed */',
        expectedOccurrences: 2,
    },
];

// --------------------------------------------------------------------------
// Step 2: each broken variant must fail a real pixel assertion with exit 42
// --------------------------------------------------------------------------

const caughtScenes = [];

for (const mut of mutations) {
    const occurrences = originalHeader.split(mut.search).length - 1;
    if (occurrences !== mut.expectedOccurrences) {
        fail(`mutation ${mut.marker}: expected ${mut.expectedOccurrences} ` +
             `source occurrence(s), found ${occurrences}; refusing to mutate`);
    }

    const workDir = fs.mkdtempSync(path.join(os.tmpdir(), `plm-${mut.marker}-`));
    const mutated = originalHeader.split(mut.search).join(mut.replace);
    // Guard: the rewritten header must actually differ and keep no original.
    if (mutated === originalHeader || mutated.includes(mut.search)) {
        fail(`mutation ${mut.marker}: rewrite did not take effect`);
    }
    fs.writeFileSync(path.join(workDir, 'pl_mpeg.h'), mutated);
    fs.copyFileSync(testSrcPath, path.join(workDir, 'coefficients.c'));

    const bin = path.join(workDir, 'coefficients');
    const build = compile(workDir, bin);
    if (build.status !== 0) {
        fs.rmSync(workDir, { recursive: true, force: true });
        fail(`${mut.marker}: variant failed to compile (does not count):\n` +
             build.stderr);
    }

    const result = run(bin, [], { cwd: workDir, timeout: 30000 });
    const output = `${result.stdout || ''}${result.stderr || ''}`;

    // A crash (signal), timeout or empty output is not a valid detection.
    if (result.error || result.signal) {
        fs.rmSync(workDir, { recursive: true, force: true });
        fail(`${mut.marker}: variant crashed/timed out (does not count)`);
    }
    const assertMatch = output.match(/COEFF_ASSERT scene=([A-Za-z0-9_]+)/);
    const ranScenes = (output.match(/^SCENE /gm) || []).length;
    if (result.status !== 42 || !assertMatch || ranScenes === 0) {
        fs.rmSync(workDir, { recursive: true, force: true });
        fail(`${mut.marker}: defect not caught (exit=${result.status}, ` +
             `assert=${assertMatch ? assertMatch[1] : 'none'}, ` +
             `scenes=${ranScenes})`);
    }

    const scene = assertMatch[1];
    const detailMatch = output.match(/COEFF_ASSERT scene=[A-Za-z0-9_]+ .*/);
    caughtScenes.push({ marker: mut.marker, scene, description: mut.description });
    console.log(`${mut.marker} location="${mut.location}"`);
    console.log(`${mut.marker} mutation="${mut.description}" ` +
                `failing_scene=${scene}`);
    if (detailMatch) {
        console.log(`${mut.marker} ${detailMatch[0]}`);
    }
    console.log(mut.marker);

    fs.rmSync(workDir, { recursive: true, force: true });
}

if (caughtScenes.length !== mutations.length) {
    fail('not every defect variant was caught');
}

// Re-confirm the canonical success line and case count.
console.log(`MPEG_COEFF_OK cases=${caseCount}`);
