#!/usr/bin/env node
/* cab_model_mutants.cjs -- mutation checks for the CAB model tests.
 *
 * Verifies that two degradations of the MSZIP decoder are caught by the
 * model-based content assertions in libmspack/test/cab_model_test.c:
 *
 *   clear-history:        every new MSZIP frame forgets the 32kB
 *                         dictionary carried over from previous frames
 *   pending-not-advanced: the stored-up output flush at the top of
 *                         mszipd_decompress() no longer advances the
 *                         pending pointer, so frame tails get re-sent
 *
 * Method: copy the sources into a fresh temporary directory per variant,
 * apply a textual mutation to the copy of mszipd.c, compile, and run the
 * three test groups.  The unmutated sources must pass all three groups
 * first.  A mutant counts as caught only when a group fails through a
 * content assertion (a CAB_MODEL_FAIL record with a first_diff offset);
 * compile failures, crashes, missing files or empty groups do not count.
 *
 * Uses only Node.js built-in modules.  Exit status 0 = both mutants
 * caught; anything else is a failure of this QA harness.
 */
'use strict';

const fs = require('fs');
const os = require('os');
const path = require('path');
const cp = require('child_process');

const repoRoot = path.resolve(__dirname, '..', '..');
const mspackDir = path.join(repoRoot, 'libmspack', 'mspack');
const testSrc = path.join(repoRoot, 'libmspack', 'test', 'cab_model_test.c');
const mainSrc = path.join(repoRoot, 'checks', 'cab_model_main.c');

const GROUPS = ['model', 'split', 'partial'];
const LIB_SRCS = ['system.c', 'cabd.c', 'lzxd.c', 'mszipd.c', 'qtmd.c'];
const CC = process.env.CC || 'cc';
const CFLAGS = ['-std=c99', '-O2', '-DHAVE_INTTYPES_H=1', '-DSIZEOF_OFF_T=8'];

const MUTANTS = [
  {
    name: 'clear-history',
    anchor: '    zip->window_posn = 0;\n    zip->bytes_output = 0;',
    replacement:
      '    zip->window_posn = 0;\n' +
      '    memset(&zip->window[0], 0, MSZIP_FRAME_SIZE); /* MUTANT: clear history */\n' +
      '    zip->bytes_output = 0;',
  },
  {
    name: 'pending-not-advanced',
    anchor:
      '  if (i) {\n' +
      '    if (zip->sys->write(zip->output, zip->o_ptr, i) != i) {\n' +
      '      return zip->error = MSPACK_ERR_WRITE;\n' +
      '    }\n' +
      '    zip->o_ptr  += i;\n' +
      '    out_bytes   -= i;\n' +
      '  }',
    replacement:
      '  if (i) {\n' +
      '    if (zip->sys->write(zip->output, zip->o_ptr, i) != i) {\n' +
      '      return zip->error = MSPACK_ERR_WRITE;\n' +
      '    }\n' +
      '    /* MUTANT: pending pointer not advanced */\n' +
      '    out_bytes   -= i;\n' +
      '  }',
  },
];

function fail(msg) {
  console.error(`cab_model_mutants: ${msg}`);
  process.exit(1);
}

/* copy the sources needed for a build into a fresh temp dir */
function makeWorkdir(tag) {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), `cabmut-${tag}-`));
  fs.mkdirSync(path.join(dir, 'mspack'));
  for (const f of fs.readdirSync(mspackDir)) {
    if (/\.(c|h)$/.test(f)) {
      fs.copyFileSync(path.join(mspackDir, f), path.join(dir, 'mspack', f));
    }
  }
  fs.copyFileSync(testSrc, path.join(dir, 'cab_model_test.c'));
  fs.copyFileSync(mainSrc, path.join(dir, 'cab_model_main.c'));
  return dir;
}

function build(dir) {
  const args = [
    ...CFLAGS,
    '-I', path.join(dir, 'mspack'),
    path.join(dir, 'cab_model_main.c'),
    path.join(dir, 'cab_model_test.c'),
    ...LIB_SRCS.map((s) => path.join(dir, 'mspack', s)),
    '-o', path.join(dir, 'model'),
  ];
  const r = cp.spawnSync(CC, args, { encoding: 'utf8' });
  if (r.status !== 0) {
    console.error(r.stdout || '');
    console.error(r.stderr || '');
    return false;
  }
  return fs.existsSync(path.join(dir, 'model'));
}

/* run the three groups; return {group: {status, signal, text}} */
function runGroups(dir) {
  const res = {};
  for (const g of GROUPS) {
    const r = cp.spawnSync(path.join(dir, 'model'), [g], {
      encoding: 'utf8',
    });
    res[g] = {
      status: r.status,
      signal: r.signal,
      text: `${r.stdout || ''}\n${r.stderr || ''}`,
    };
  }
  return res;
}

/* the unmutated build must pass every group with > 0 assertions */
function checkPristine(results) {
  for (const g of GROUPS) {
    const r = results[g];
    if (r.status !== 0 || r.signal !== null) {
      fail(`pristine run of group ${g} failed (status=${r.status} signal=${r.signal})\n${r.text}`);
    }
    const m = r.text.match(new RegExp(`^CAB_MODEL_GROUP ${g} assertions=([0-9]+)$`, 'm'));
    if (!m || Number(m[1]) <= 0) {
      fail(`pristine run of group ${g} did not report positive assertions\n${r.text}`);
    }
    if (!/^CAB_MODEL_GROUP_COMPLETE$/m.test(r.text)) {
      fail(`pristine run of group ${g} did not complete\n${r.text}`);
    }
  }
}

/* a mutant is caught iff at least one group fails cleanly (exit status
 * 3 from checks/cab_model_main.c) through a content assertion, i.e. a
 * CAB_MODEL_FAIL record carrying a first_diff offset; no group may
 * crash, and compile failures never count */
function mutantCaught(results) {
  let caught = false;
  for (const g of GROUPS) {
    const r = results[g];
    if (r.signal !== null || (r.status !== 0 && r.status !== 3)) {
      console.error(`group ${g}: abnormal exit status=${r.status} signal=${r.signal}`);
      return false;
    }
    if (r.status === 3) {
      if (/^CAB_MODEL_FAIL .*first_diff=[0-9]+/m.test(r.text)) {
        caught = true;
      }
      else {
        console.error(`group ${g}: failed without a content assertion\n${r.text}`);
        return false;
      }
    }
  }
  return caught;
}

function applyMutation(dir, mutant) {
  const file = path.join(dir, 'mspack', 'mszipd.c');
  const src = fs.readFileSync(file, 'utf8');
  const first = src.indexOf(mutant.anchor);
  if (first < 0) fail(`mutant ${mutant.name}: anchor not found in mszipd.c`);
  if (src.indexOf(mutant.anchor, first + 1) >= 0) {
    fail(`mutant ${mutant.name}: anchor is not unique in mszipd.c`);
  }
  const mutated = src.replace(mutant.anchor, mutant.replacement);
  if (mutated === src) fail(`mutant ${mutant.name}: replacement had no effect`);
  fs.writeFileSync(file, mutated);
}

const workdirs = [];
try {
  /* 1. pristine sources must pass all three groups */
  const clean = makeWorkdir('clean');
  workdirs.push(clean);
  if (!build(clean)) fail('pristine sources did not compile');
  checkPristine(runGroups(clean));

  /* 2. each mutant must be caught by the content assertions */
  for (const mutant of MUTANTS) {
    const dir = makeWorkdir(mutant.name);
    workdirs.push(dir);
    applyMutation(dir, mutant);
    if (!build(dir)) fail(`mutant ${mutant.name}: mutated sources did not compile`);
    if (!mutantCaught(runGroups(dir))) {
      fail(`mutant ${mutant.name}: not caught by content assertions`);
    }
    console.log(`MUTANT ${mutant.name} caught`);
  }
}
finally {
  for (const dir of workdirs) {
    fs.rmSync(dir, { recursive: true, force: true });
  }
}
