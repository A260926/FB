'use strict';

// Regression tests for the bilingual two-track alignment model.
// All expected values below are hand-computed from the contract in
// task_support/alignment_contract.md (integer millisecond costs, envelope
// matching, transition priority 11 < 12 < 21 < 10 < 01).

const { checked } = require('../task_support/check.cjs');
const { execFileSync, spawnSync } = require('child_process');
const fs = require('fs');
const os = require('os');
const path = require('path');
const {
  WebVTT,
  VTTCue,
  VTTRegion,
  BilingualAlignment
} = require('../lib/index.js');

function makeCue(startMs, endMs, text) {
  return new VTTCue(startMs / 1000, endMs / 1000, text === undefined ? 'x' : text);
}

// Build a track of {key: prefix+N, cue} entries from [startMs, endMs] pairs.
function track(prefix, spec) {
  return spec.map(([start, end], index) => ({
    key: prefix + (index + 1),
    cue: makeCue(start, end)
  }));
}

// Return the error message a function throws, or null when it does not.
function errorOf(fn) {
  try {
    fn();
  } catch (error) {
    return error.message;
  }
  return null;
}

checked('optimal_path', (check) => {
  // Greedy nearest-neighbour would pair l2-r1 (cost 150) and skip l1 for a
  // total of 2150; the global optimum merges both left cues into one 2:1
  // block for 1050 + 100 + 500 = 1650.
  {
    const model = BilingualAlignment.align(
      track('l', [[0, 1000], [1100, 2100]]),
      track('r', [[1050, 2000]]));
    check(model.groups, [{ left: ['l1', 'l2'], right: ['r1'], locked: false }]);
    check(model.cost, 1650);
  }

  // Exact tie between matching (2000 + 2000 = 4000) and skipping both
  // sides (2000 + 2000): the sequence [11] beats [10, 01] because 11 < 10.
  {
    const model = BilingualAlignment.align(
      track('l', [[0, 1000]]),
      track('r', [[2000, 3000]]));
    check(model.groups, [{ left: ['l1'], right: ['r1'], locked: false }]);
    check(model.cost, 4000);
  }

  // Exact tie between [11, 01] (0 + 2000) and [12] (0 + 1500 + 500):
  // 11 < 12, so the 1:1 match plus the unmatched right cue wins.
  {
    const model = BilingualAlignment.align(
      track('l', [[0, 1000]]),
      track('r', [[0, 1000], [2000, 2500]]));
    check(model.groups, [
      { left: ['l1'], right: ['r1'], locked: false },
      { left: [], right: ['r2'], locked: false }
    ]);
    check(model.cost, 2000);
  }

  // A 1:2 merge wins outright: 0 + 1400 + 500 = 1900 beats 0 + 2000.
  {
    const model = BilingualAlignment.align(
      track('l', [[0, 1000]]),
      track('r', [[0, 1000], [2000, 2400]]));
    check(model.groups, [{ left: ['l1'], right: ['r1', 'r2'], locked: false }]);
    check(model.cost, 1900);
  }

  // A 2:1 merge wins outright: 0 + 0 + 500 = 500 beats 1500 + 2000.
  {
    const model = BilingualAlignment.align(
      track('l', [[0, 1000], [1500, 2500]]),
      track('r', [[0, 2500]]));
    check(model.groups, [{ left: ['l1', 'l2'], right: ['r1'], locked: false }]);
    check(model.cost, 500);
  }

  // An unmatched left entry is skipped for exactly one gapPenalty.
  {
    const model = BilingualAlignment.align(
      track('l', [[0, 1000], [8000, 9000]]),
      track('r', [[0, 1000]]));
    check(model.groups, [
      { left: ['l1'], right: ['r1'], locked: false },
      { left: ['l2'], right: [], locked: false }
    ]);
    check(model.cost, 2000);
  }

  // Empty tracks are legal.
  {
    const model = BilingualAlignment.align([], []);
    check(model.groups, []);
    check(model.cost, 0);
    const rightOnly = BilingualAlignment.align([], track('r', [[0, 1000]]));
    check(rightOnly.groups, [{ left: [], right: ['r1'], locked: false }]);
    check(rightOnly.cost, 2000);
  }

  // Locks partition the tracks: the locked 1:2 block is kept verbatim and
  // the segments before and after it are solved on their own. Without the
  // lock the same tracks align perfectly for cost 0.
  {
    const left = track('l', [[0, 1000], [2000, 3000], [4000, 5000]]);
    const right = track('r', [[0, 1000], [2000, 3000], [4000, 5000]]);
    const unlocked = BilingualAlignment.align(left, right);
    check(unlocked.groups, [
      { left: ['l1'], right: ['r1'], locked: false },
      { left: ['l2'], right: ['r2'], locked: false },
      { left: ['l3'], right: ['r3'], locked: false }
    ]);
    check(unlocked.cost, 0);
    const locked = BilingualAlignment.align(left, right, {
      locks: [{ left: ['l2'], right: ['r2', 'r3'] }]
    });
    check(locked.groups, [
      { left: ['l1'], right: ['r1'], locked: false },
      { left: ['l2'], right: ['r2', 'r3'], locked: true },
      { left: ['l3'], right: [], locked: false }
    ]);
    // 0 + (0 + 2000 + 500) + 2000 = 4500.
    check(locked.cost, 4500);
    check(locked.locks, [{ left: ['l2'], right: ['r2', 'r3'] }]);
  }

  // align() must not modify the caller's arrays, cues or options.
  {
    const left = track('l', [[0, 1000]]);
    const right = track('r', [[0, 1000]]);
    const options = { gapPenalty: 1000, locks: [] };
    const model = BilingualAlignment.align(left, right, options);
    check(left.length, 1);
    check(right.length, 1);
    check(left[0].cue.startTime, 0);
    check(left[0].cue.hasBeenReset, false);
    check(Object.keys(options), ['gapPenalty', 'locks']);
    check(options.locks.length, 0);
    check(model.cost, 0);
  }

  // Invalid inputs and invalid locks are rejected, never relaxed.
  {
    check(
      errorOf(() => BilingualAlignment.align(
        [{ key: 'l1', cue: makeCue(0, 1000) }, { key: 'l1', cue: makeCue(2000, 3000) }],
        [])),
      'BilingualAlignment: left entry 1 duplicates key "l1"');

    const shared = makeCue(0, 1000);
    check(
      errorOf(() => BilingualAlignment.align(
        [{ key: 'l1', cue: shared }], [{ key: 'r1', cue: shared }])),
      'BilingualAlignment: each cue object may only be used once across both tracks');

    check(
      errorOf(() => BilingualAlignment.align(
        [{ key: 'l1', cue: {} }], [])),
      'BilingualAlignment: left entry 0 ("l1") cue must be a VTTCue');

    check(
      errorOf(() => BilingualAlignment.align(
        [{ key: 'l1', cue: makeCue(0, 1000.4) }], [])),
      'BilingualAlignment: left entry "l1" has an invalid endTime');

    check(
      errorOf(() => BilingualAlignment.align(
        [{ key: 'l1', cue: makeCue(1000, 1000) }], [])),
      'BilingualAlignment: left entry "l1" must have endTime greater than startTime');

    check(
      errorOf(() => BilingualAlignment.align(
        track('l', [[2000, 3000], [0, 1000]]), [])),
      'BilingualAlignment: left start times must be non-decreasing');

    check(
      errorOf(() => BilingualAlignment.align([], [], { gapPenalty: 1.5 })),
      'BilingualAlignment: options.gapPenalty must be an integer in [0, 1000000]');

    check(
      errorOf(() => BilingualAlignment.align([], [], { mergePenalty: -1 })),
      'BilingualAlignment: options.mergePenalty must be an integer in [0, 1000000]');

    const left = track('l', [[0, 1000], [2000, 3000], [4000, 5000]]);
    const right = track('r', [[0, 1000], [2000, 3000], [4000, 5000]]);
    check(
      errorOf(() => BilingualAlignment.align(left, right, {
        locks: [{ left: ['nope'], right: ['r1'] }]
      })),
      'BilingualAlignment: locks[0].left references unknown key "nope"');
    check(
      errorOf(() => BilingualAlignment.align(left, right, {
        locks: [{ left: ['l1', 'l3'], right: ['r1'] }]
      })),
      'BilingualAlignment: locks[0].left keys must be consecutive and in track order');
    check(
      errorOf(() => BilingualAlignment.align(left, right, {
        locks: [{ left: ['l1', 'l1'], right: ['r1'] }]
      })),
      'BilingualAlignment: locks[0].left keys must be consecutive and in track order');
    check(
      errorOf(() => BilingualAlignment.align(left, right, {
        locks: [{ left: ['l1', 'l2'], right: ['r1', 'r2'] }]
      })),
      'BilingualAlignment: locks[0] must reference one or two cues per side (1:1, 1:2 or 2:1)');
    check(
      errorOf(() => BilingualAlignment.align(left, right, {
        locks: [{ left: ['l2'], right: ['r2'] }, { left: ['l1'], right: ['r1'] }]
      })),
      'BilingualAlignment: locks[1] crosses, overlaps or is out of order with an earlier lock');
    check(
      errorOf(() => BilingualAlignment.align(left, right, {
        locks: [{ left: ['l1', 'l2'], right: ['r1'] }, { left: ['l2'], right: ['r2'] }]
      })),
      'BilingualAlignment: locks[1] crosses, overlaps or is out of order with an earlier lock');
  }
});

checked('edit_propagation', (check) => {
  // Shared fixture: l1 [1,3] matches r1+r2 [1.1,2.6] (cost 1000), l2 [5,6]
  // matches r3 [5,6] (cost 0).
  function buildEditModel() {
    const left = track('l', [[1000, 3000], [5000, 6000]]);
    const right = track('r', [[1100, 1600], [1700, 2600], [5000, 6000]]);
    return { left, right, model: BilingualAlignment.align(left, right) };
  }
  const expectedEditGroups = [
    { left: ['l1'], right: ['r1', 'r2'], locked: false },
    { left: ['l2'], right: ['r3'], locked: false }
  ];

  // Affine mapping of a 1:2 group: envelope [1100, 2600] -> [1000, 3000].
  // r1: 1100 -> 1000, 1600 -> 1000 + floor(500 * 2000 / 1500) = 1666.
  // r2: 1700 -> 1000 + floor(600 * 2000 / 1500) = 1800, 2600 -> 3000.
  {
    const { left, right, model } = buildEditModel();
    check(model.groups, expectedEditGroups);
    check(model.cost, 1000);
    const sentinel = {};
    right[0].cue.displayState = sentinel;
    model.conform(0);
    check(right[0].cue.startTime, 1);
    check(right[0].cue.endTime, 1.666);
    check(right[1].cue.startTime, 1.8);
    check(right[1].cue.endTime, 3);
    // Without ripple the successor is untouched.
    check(right[2].cue.startTime, 5);
    check(right[2].cue.endTime, 6);
    // Edits go through the setters; untouched cues stay clean.
    check(right[0].cue.hasBeenReset, true);
    check(right[1].cue.hasBeenReset, true);
    check(right[2].cue.hasBeenReset, false);
    check(left[0].cue.hasBeenReset, false);
    check(left[1].cue.hasBeenReset, false);
    // displayState is preserved so processCues invalidates via the reset flag.
    check(right[0].cue.displayState, sentinel);
    // Groups do not move; the cost follows the new cue values.
    check(model.groups, expectedEditGroups);
    check(model.cost, 500);
  }

  // Ripple shifts every later right entry by the end delta (3000 - 2600 =
  // 400 ms); a second conform then works on the updated values.
  {
    const { right, model } = buildEditModel();
    model.conform(0, true);
    check(right[0].cue.startTime, 1);
    check(right[0].cue.endTime, 1.666);
    check(right[1].cue.startTime, 1.8);
    check(right[1].cue.endTime, 3);
    check(right[2].cue.startTime, 5.4);
    check(right[2].cue.endTime, 6.4);
    // 500 (group 0) + 400 + 400 (group 1 envelopes) = 1300.
    check(model.cost, 1300);
    // Consecutive edit: group 1 now maps [5400, 6400] -> [5000, 6000].
    model.conform(1);
    check(right[2].cue.startTime, 5);
    check(right[2].cue.endTime, 6);
    check(model.cost, 500);
    check(model.groups, expectedEditGroups);
  }

  // A cue with an inline timestamp tag rejects the whole edit; a plain
  // timestamp-looking text without angle brackets is fine.
  {
    const { right, model } = buildEditModel();
    right[1].cue.text = 'x <00:01.000> y';
    right[1].cue.hasBeenReset = false;
    check(errorOf(() => model.conform(0)),
      'BilingualAlignment: cue "r2" contains an inline timestamp tag');
    // Nothing changed: times, text, cache flags, groups and cost.
    check(right[0].cue.startTime, 1.1);
    check(right[0].cue.endTime, 1.6);
    check(right[1].cue.startTime, 1.7);
    check(right[1].cue.endTime, 2.6);
    check(right[2].cue.startTime, 5);
    check(right[1].cue.text, 'x <00:01.000> y');
    check(right[0].cue.hasBeenReset, false);
    check(right[1].cue.hasBeenReset, false);
    check(model.groups, expectedEditGroups);
    check(model.cost, 1000);
    right[1].cue.text = 'meet at 00:01.000 sharp';
    model.conform(0);
    check(right[1].cue.startTime, 1.8);
    check(right[1].cue.endTime, 3);
  }

  // The mapping may not collapse a cue to zero duration.
  {
    const left = track('l', [[1000, 1001]]);
    const right = track('r', [[0, 1], [1, 1000]]);
    const model = BilingualAlignment.align(left, right);
    check(model.groups, [{ left: ['l1'], right: ['r1', 'r2'], locked: false }]);
    // 1000 + 1 + 500 = 1501.
    check(model.cost, 1501);
    check(errorOf(() => model.conform(0)),
      'BilingualAlignment: conform would collapse cue "r1" to a non-positive duration');
    check(right[0].cue.startTime, 0);
    check(right[0].cue.endTime, 0.001);
    check(right[1].cue.startTime, 0.001);
    check(right[1].cue.endTime, 1);
    check(right[0].cue.hasBeenReset, false);
    check(model.cost, 1501);
  }

  // Ripple may not break the right track start order (an overlapping
  // successor shifted before the group's mapped cues).
  {
    const left = track('l', [[0, 7000]]);
    const right = track('r', [[0, 10000], [9000, 9500], [9000, 9600]]);
    const model = BilingualAlignment.align(left, right);
    check(model.groups, [
      { left: ['l1'], right: ['r1', 'r2'], locked: false },
      { left: [], right: ['r3'], locked: false }
    ]);
    // 3000 + 500 + 2000 = 5500.
    check(model.cost, 5500);
    // r2 maps to [6300, 6650]; r3 shifts by -3000 to [6000, 6600] and
    // would start before r2.
    check(errorOf(() => model.conform(0, true)),
      'BilingualAlignment: conform would break the right track start order');
    check(right[0].cue.startTime, 0);
    check(right[0].cue.endTime, 10);
    check(right[1].cue.startTime, 9);
    check(right[2].cue.startTime, 9);
    check(right[0].cue.hasBeenReset, false);
    check(model.cost, 5500);
  }

  // Ripple may not push a successor to a negative time.
  {
    const left = track('l', [[0, 8000]]);
    const right = track('r', [[0, 10000], [100, 200], [150, 300]]);
    const model = BilingualAlignment.align(left, right);
    check(model.groups, [
      { left: ['l1'], right: ['r1', 'r2'], locked: false },
      { left: [], right: ['r3'], locked: false }
    ]);
    // 2000 + 500 + 2000 = 4500.
    check(model.cost, 4500);
    // The shift is 8000 - 10000 = -2000; r3 would start at -1850.
    check(errorOf(() => model.conform(0, true)),
      'BilingualAlignment: conform would move a cue outside the [0, 1000000000] ms range');
    check(right[2].cue.startTime, 0.15);
    check(right[2].cue.endTime, 0.3);
    check(model.cost, 4500);
  }

  // Bad conform arguments are rejected without side effects.
  {
    const { right, model } = buildEditModel();
    check(errorOf(() => model.conform(0.5)),
      'BilingualAlignment: groupIndex must be an integer');
    check(errorOf(() => model.conform(-1)),
      'BilingualAlignment: groupIndex -1 is out of range');
    check(errorOf(() => model.conform(99)),
      'BilingualAlignment: groupIndex 99 is out of range');
    check(errorOf(() => model.conform(0, 'yes')),
      'BilingualAlignment: ripple must be a boolean');
    check(right[0].cue.startTime, 1.1);
    check(model.cost, 1000);
  }

  // Skip groups cannot be conformed.
  {
    const model = BilingualAlignment.align(
      track('l', [[0, 1000]]),
      track('r', [[0, 1000], [2000, 2500]]));
    check(errorOf(() => model.conform(1)),
      'BilingualAlignment: group 1 does not reference both tracks');
  }

  // realign() re-reads externally edited cue times and re-solves; a tie
  // between [11, 10, 01] and [11, 01, 10] goes to 10 < 01.
  {
    const left = track('l', [[0, 1000], [2000, 3000]]);
    const right = track('r', [[0, 1000], [2000, 3000]]);
    const model = BilingualAlignment.align(left, right);
    check(model.cost, 0);
    right[1].cue.startTime = 8;
    right[1].cue.endTime = 9;
    model.realign();
    check(model.groups, [
      { left: ['l1'], right: ['r1'], locked: false },
      { left: ['l2'], right: [], locked: false },
      { left: [], right: ['r2'], locked: false }
    ]);
    check(model.cost, 4000);
  }

  // realign() keeps manual locks by key, and keeps the current groups when
  // the current cue values no longer satisfy the input contract.
  {
    const left = track('l', [[0, 1000], [2000, 3000]]);
    const right = track('r', [[0, 1000], [2000, 3000]]);
    const model = BilingualAlignment.align(left, right, {
      locks: [{ left: ['l1'], right: ['r1'] }]
    });
    check(model.groups, [
      { left: ['l1'], right: ['r1'], locked: true },
      { left: ['l2'], right: ['r2'], locked: false }
    ]);
    right[1].cue.startTime = 8;
    right[1].cue.endTime = 9;
    model.realign();
    check(model.groups, [
      { left: ['l1'], right: ['r1'], locked: true },
      { left: ['l2'], right: [], locked: false },
      { left: [], right: ['r2'], locked: false }
    ]);
    check(model.cost, 4000);
    left[0].cue.startTime = 5;
    left[0].cue.endTime = 1;
    check(errorOf(() => model.realign()),
      'BilingualAlignment: left entry "l1" must have endTime greater than startTime');
    check(model.groups, [
      { left: ['l1'], right: ['r1'], locked: true },
      { left: ['l2'], right: [], locked: false },
      { left: [], right: ['r2'], locked: false }
    ]);
  }
});

checked('persistence', async (check) => {
  // regionA is shared by both left cues; regionB (equal properties but a
  // distinct object) is shared by r1 and r3; r2 has no region.
  function buildRegionModel() {
    const regionA = new VTTRegion();
    regionA.width = 45;
    regionA.lines = 2;
    const regionB = new VTTRegion();
    regionB.width = 45;
    regionB.lines = 2;
    const l1 = makeCue(1000, 3000, '你好，世界');
    l1.id = 'dup';
    l1.region = regionA;
    const l2 = makeCue(5000, 6000, 'second');
    l2.id = 'dup';
    l2.region = regionA;
    const r1 = makeCue(1100, 1600, 'one');
    r1.id = 'x';
    r1.region = regionB;
    const r2 = makeCue(1700, 2600, 'two');
    r2.id = 'y';
    r2.position = 30;
    r2.align = 'end';
    r2.pauseOnExit = true;
    const r3 = makeCue(5000, 6000, 'three');
    r3.id = 'z';
    r3.region = regionB;
    return BilingualAlignment.align(
      [{ key: 'l1', cue: l1 }, { key: 'l2', cue: l2 }],
      [{ key: 'r1', cue: r1 }, { key: 'r2', cue: r2 }, { key: 'r3', cue: r3 }]);
  }

  const defaultCue = {
    pauseOnExit: false,
    vertical: '',
    snapToLines: true,
    line: 'auto',
    lineAlign: 'start',
    position: 'auto',
    positionAlign: 'auto',
    size: 100,
    align: 'center'
  };
  const expectedDoc = {
    version: 1,
    options: { gapPenalty: 2000, mergePenalty: 500 },
    regions: [
      { width: 45, lines: 2, regionAnchorX: 0, regionAnchorY: 100, viewportAnchorX: 0, viewportAnchorY: 100, scroll: '' },
      { width: 45, lines: 2, regionAnchorX: 0, regionAnchorY: 100, viewportAnchorX: 0, viewportAnchorY: 100, scroll: '' }
    ],
    left: [
      { key: 'l1', cue: Object.assign({}, defaultCue, { id: 'dup', startTime: 1, endTime: 3, text: '你好，世界', region: 0 }) },
      { key: 'l2', cue: Object.assign({}, defaultCue, { id: 'dup', startTime: 5, endTime: 6, text: 'second', region: 0 }) }
    ],
    right: [
      { key: 'r1', cue: Object.assign({}, defaultCue, { id: 'x', startTime: 1.1, endTime: 1.6, text: 'one', region: 1 }) },
      { key: 'r2', cue: Object.assign({}, defaultCue, { id: 'y', startTime: 1.7, endTime: 2.6, text: 'two', pauseOnExit: true, position: 30, align: 'end', region: null }) },
      { key: 'r3', cue: Object.assign({}, defaultCue, { id: 'z', startTime: 5, endTime: 6, text: 'three', region: 1 }) }
    ],
    locks: [],
    groups: [
      { left: ['l1'], right: ['r1', 'r2'], locked: false },
      { left: ['l2'], right: ['r3'], locked: false }
    ]
  };

  // toJSON writes the versioned document with the region reference table.
  {
    const model = buildRegionModel();
    check(model.toJSON(), expectedDoc);
  }

  // Round-trip: groups, cost, cue values and region identity survive;
  // cache fields do not.
  {
    const model = buildRegionModel();
    const document = JSON.parse(JSON.stringify(model.toJSON()));
    const restored = BilingualAlignment.fromJSON(document);
    check(restored.groups, expectedDoc.groups);
    check(restored.cost, 1000);
    check(restored.toJSON(), expectedDoc);
    const rl = restored.left;
    const rr = restored.right;
    check(rl[0].cue instanceof VTTCue, true);
    check(rl[0].cue.region instanceof VTTRegion, true);
    check(rl[0].cue.region === rl[1].cue.region, true);
    check(rr[0].cue.region === rr[2].cue.region, true);
    // Equal properties do not merge the two region objects.
    check(rl[0].cue.region === rr[0].cue.region, false);
    check(rr[1].cue.region, null);
    check(rl[0].cue.region.width, 45);
    check(rl[0].cue.region.lines, 2);
    // Original ids (duplicates included), text and layout properties.
    check(rl[0].cue.text, '你好，世界');
    check(rl[0].cue.id, 'dup');
    check(rl[1].cue.id, 'dup');
    check(rr[1].cue.position, 30);
    check(rr[1].cue.align, 'end');
    check(rr[1].cue.pauseOnExit, true);
    // Cache fields are not persisted.
    check(rl[0].cue.hasBeenReset, false);
    check(rl[0].cue.displayState, undefined);
    // Two independent restores share nothing.
    const again = BilingualAlignment.fromJSON(document);
    check(again.left[0].cue === restored.left[0].cue, false);
    check(again.left[0].cue.region === restored.left[0].cue.region, false);
    // The restored model keeps editing: conform group 0 in the new state.
    restored.conform(0);
    check(rr[0].cue.startTime, 1);
    check(rr[0].cue.endTime, 1.666);
    check(rr[1].cue.startTime, 1.8);
    check(restored.cost, 500);
  }

  // fromJSON rejects unknown versions, missing fields, bad region
  // references and illegal or incomplete group paths.
  {
    const document = JSON.parse(JSON.stringify(buildRegionModel().toJSON()));

    check(errorOf(() => BilingualAlignment.fromJSON(
      Object.assign({}, document, { version: 2 }))),
      'BilingualAlignment: unsupported document version 2');

    const noGroups = JSON.parse(JSON.stringify(document));
    delete noGroups.groups;
    check(errorOf(() => BilingualAlignment.fromJSON(noGroups)),
      'BilingualAlignment: document is missing field "groups"');

    const badRef = JSON.parse(JSON.stringify(document));
    badRef.right[0].cue.region = 5;
    check(errorOf(() => BilingualAlignment.fromJSON(badRef)),
      'BilingualAlignment: right[0] cue has a bad region reference 5');

    const incomplete = JSON.parse(JSON.stringify(document));
    incomplete.groups = incomplete.groups.slice(0, 1);
    check(errorOf(() => BilingualAlignment.fromJSON(incomplete)),
      'BilingualAlignment: groups do not form a complete consumption of both tracks');

    const wrongLock = JSON.parse(JSON.stringify(document));
    wrongLock.groups[0].locked = true;
    check(errorOf(() => BilingualAlignment.fromJSON(wrongLock)),
      'BilingualAlignment: groups[0]: locked group does not match locks[0]');

    const extraLock = JSON.parse(JSON.stringify(document));
    extraLock.locks = [{ left: ['l2'], right: ['r3'] }];
    check(errorOf(() => BilingualAlignment.fromJSON(extraLock)),
      'BilingualAlignment: not every lock appears as a locked group');
  }

  // Locks survive a round-trip and still partition the solve.
  {
    const left = track('l', [[0, 1000], [2000, 3000], [4000, 5000]]);
    const right = track('r', [[0, 1000], [2000, 3000], [4000, 5000]]);
    const model = BilingualAlignment.align(left, right, {
      locks: [{ left: ['l2'], right: ['r2', 'r3'] }]
    });
    const restored = BilingualAlignment.fromJSON(
      JSON.parse(JSON.stringify(model.toJSON())));
    check(restored.groups, [
      { left: ['l1'], right: ['r1'], locked: false },
      { left: ['l2'], right: ['r2', 'r3'], locked: true },
      { left: ['l3'], right: [], locked: false }
    ]);
    check(restored.cost, 4500);
    check(restored.locks, [{ left: ['l2'], right: ['r2', 'r3'] }]);
  }

  // Cross-process: the example CLI aligns two WebVTT files, then a second
  // process restores the model and conforms it. Chinese text and
  // duplicate original cue ids survive the trip.
  {
    const tmp = fs.mkdtempSync(path.join(os.tmpdir(), 'bilingual-'));
    const leftVtt = path.join(tmp, 'left.vtt');
    const rightVtt = path.join(tmp, 'right.vtt');
    const alignOut = path.join(tmp, 'align.json');
    const conformOut = path.join(tmp, 'conform.json');
    fs.writeFileSync(leftVtt,
      'WEBVTT\n' +
      '\n' +
      'dup\n' +
      '00:01.000 --> 00:03.000\n' +
      '你好，世界\n' +
      '\n' +
      'dup\n' +
      '00:05.000 --> 00:06.000\n' +
      'second\n');
    fs.writeFileSync(rightVtt,
      'WEBVTT\n' +
      '\n' +
      'x\n' +
      '00:01.100 --> 00:01.600\n' +
      'one\n' +
      '\n' +
      'y\n' +
      '00:01.700 --> 00:02.600\n' +
      'two\n' +
      '\n' +
      'z\n' +
      '00:05.000 --> 00:06.000\n' +
      'three\n');
    const example = path.join(__dirname, '..', 'examples', 'bilingual.cjs');

    execFileSync(process.execPath, [example, 'align', leftVtt, rightVtt, alignOut]);
    const aligned = JSON.parse(fs.readFileSync(alignOut, 'utf8'));
    check(aligned.left.map((entry) => entry.key), ['l1', 'l2']);
    check(aligned.right.map((entry) => entry.key), ['r1', 'r2', 'r3']);
    check(aligned.groups, expectedDoc.groups);
    check(aligned.left[0].cue.text, '你好，世界');
    check(aligned.left[0].cue.id, 'dup');
    check(aligned.left[1].cue.id, 'dup');
    check(aligned.locks, []);

    // Restore and edit in a second process.
    execFileSync(process.execPath, [example, 'conform', alignOut, '0', conformOut, '--ripple']);
    const conformed = JSON.parse(fs.readFileSync(conformOut, 'utf8'));
    check(conformed.groups, aligned.groups);
    check(conformed.right[0].cue.startTime, 1);
    check(conformed.right[0].cue.endTime, 1.666);
    check(conformed.right[1].cue.startTime, 1.8);
    check(conformed.right[1].cue.endTime, 3);
    check(conformed.right[2].cue.startTime, 5.4);
    check(conformed.right[2].cue.endTime, 6.4);
    const restored = BilingualAlignment.fromJSON(conformed);
    check(restored.cost, 1300);

    // Failures exit non-zero and never overwrite an existing output.
    const sentinelOut = path.join(tmp, 'sentinel.json');
    fs.writeFileSync(sentinelOut, 'keep me');
    const badGroup = spawnSync(process.execPath,
      [example, 'conform', alignOut, '99', sentinelOut]);
    check(badGroup.status !== 0, true);
    check(fs.readFileSync(sentinelOut, 'utf8'), 'keep me');

    const missingOut = path.join(tmp, 'missing.json');
    const badInput = spawnSync(process.execPath,
      [example, 'align', path.join(tmp, 'nope.vtt'), rightVtt, missingOut]);
    check(badInput.status !== 0, true);
    check(fs.existsSync(missingOut), false);
  }
});
