/**
 * Copyright 2013 vtt.js Contributors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

// BilingualAlignment: a two-track cue association model for reviewing
// bilingual (side-by-side) subtitles.
//
// Two tracks of {key, cue} entries (keys unique per track, cues are this
// library's VTTCue) are associated into groups of shape 1:1, 1:2, 2:1 or
// single-side skips (1:0, 0:1). The automatic association minimizes the
// total cost over the whole tracks; it never greedily picks the nearest
// cue. Reviewers may pin association blocks with options.locks; locked
// blocks partition the tracks and the segments in between are solved
// independently. Costs are computed in integer milliseconds:
//
//   match(group) = |leftEnvStart - rightEnvStart|
//                + |leftEnvEnd   - rightEnvEnd|
//                + mergePenalty * (leftCount + rightCount - 2)
//   skip(entry)  = gapPenalty
//
// where an envelope is [min start, max end] of the group's cues. Ties are
// broken by the lexicographic order of the full transition sequence with
// the fixed transition priority 11 < 12 < 21 < 10 < 01.
//
// conform(groupIndex, ripple) affine-maps the right cues of a matched
// group from their old envelope onto the left envelope, in exact integer
// milliseconds (BigInt), and optionally shifts every later right-track
// entry by the resulting end-point delta. Edits go through the native cue
// setters so hasBeenReset/displayState behave as usual; the whole edit is
// computed and validated before anything is written back.
//
// toJSON() serializes a versioned document:
//   {
//     version: 1,
//     options: {gapPenalty, mergePenalty},
//     regions: [{width, lines, regionAnchorX, regionAnchorY,
//                viewportAnchorX, viewportAnchorY, scroll}, ...],
//     left:  [{key, cue: {id, startTime, endTime, text, pauseOnExit,
//             vertical, snapToLines, line, lineAlign, position,
//             positionAlign, size, align, region}}, ...],
//     right: [...],
//     locks: [{left: [key], right: [key]}, ...],
//     groups: [{left: [key], right: [key], locked}, ...]
//   }
// Cue times are stored in seconds. cue.region is an index into `regions`
// (or null); cues that shared one region object share an index, and two
// distinct regions are never merged even when all properties are equal.
// Cache fields (hasBeenReset, displayState) are not persisted; restored
// cues and regions are created through the native constructors and
// setters and start with a clean cache state.

"use strict";

var VTTCue = require("./vttcue.js");
var VTTRegion = require("./vttregion.js");

var FORMAT_VERSION = 1;
var DEFAULT_GAP_PENALTY = 2000;
var DEFAULT_MERGE_PENALTY = 500;
var MAX_PENALTY = 1e6;
var MAX_TIME_MS = 1e9;
var MAX_ENTRIES = 200;
var MS_TOLERANCE = 1e-6;

// A complete WebVTT inline timestamp tag: <mm:ss.mmm> or <hh:mm:ss.mmm>
// (hours may use more than two digits).
var INLINE_TIMESTAMP = /<(?:\d{2,}:)?\d{2}:\d{2}\.\d{3}>/;

var REGION_FIELDS = ["width", "lines", "regionAnchorX", "regionAnchorY",
                     "viewportAnchorX", "viewportAnchorY", "scroll"];
var CUE_FIELDS = ["id", "startTime", "endTime", "text", "pauseOnExit",
                  "vertical", "snapToLines", "line", "lineAlign", "position",
                  "positionAlign", "size", "align", "region"];

function fail(message) {
  throw new Error("BilingualAlignment: " + message);
}

function isInteger(value) {
  return typeof value === "number" && isFinite(value) &&
         Math.floor(value) === value;
}

// Convert a cue time in seconds to integer milliseconds, or null when the
// value does not satisfy the contract (finite, millisecond precision and
// inside [0, 1e9] ms).
function toMs(seconds) {
  if (typeof seconds !== "number" || !isFinite(seconds)) {
    return null;
  }
  var scaled = seconds * 1000;
  var ms = Math.round(scaled);
  if (Math.abs(scaled - ms) > MS_TOLERANCE) {
    return null;
  }
  if (ms < 0 || ms > MAX_TIME_MS) {
    return null;
  }
  return ms;
}

// Read and validate the current millisecond times of a track's cues:
// finite, millisecond precision, in range, positive duration and
// non-decreasing start times. `where` is "left" or "right".
function readTrackTimes(entries, where) {
  var times = [];
  var prevStart = -1;
  for (var i = 0; i < entries.length; i++) {
    var key = entries[i].key;
    var cue = entries[i].cue;
    var start = toMs(cue.startTime);
    var end = toMs(cue.endTime);
    if (start === null) {
      fail(where + " entry \"" + key + "\" has an invalid startTime");
    }
    if (end === null) {
      fail(where + " entry \"" + key + "\" has an invalid endTime");
    }
    if (end <= start) {
      fail(where + " entry \"" + key +
           "\" must have endTime greater than startTime");
    }
    if (start < prevStart) {
      fail(where + " start times must be non-decreasing");
    }
    prevStart = start;
    times.push({start: start, end: end});
  }
  return times;
}

// Validate the {key, cue} entries given to align(). Returns a fresh array
// of {key, cue} pairs; the caller's array is never modified.
function validateTrackInput(entries, where) {
  if (!Array.isArray(entries)) {
    fail(where + " must be an array of {key, cue} entries");
  }
  if (entries.length > MAX_ENTRIES) {
    fail(where + " must not have more than " + MAX_ENTRIES + " entries");
  }
  var seenKeys = Object.create(null);
  var result = [];
  for (var i = 0; i < entries.length; i++) {
    var entry = entries[i];
    if (!entry || typeof entry !== "object" || Array.isArray(entry)) {
      fail(where + " entry " + i + " must be an object with key and cue");
    }
    var key = entry.key;
    var cue = entry.cue;
    if (typeof key !== "string" || key === "") {
      fail(where + " entry " + i + " must have a non-empty string key");
    }
    if (seenKeys[key]) {
      fail(where + " entry " + i + " duplicates key \"" + key + "\"");
    }
    seenKeys[key] = true;
    if (!(cue instanceof VTTCue)) {
      fail(where + " entry " + i + " (\"" + key + "\") cue must be a VTTCue");
    }
    result.push({key: key, cue: cue});
  }
  return result;
}

// Every cue object may appear at most once across both tracks.
function assertDistinctCues(leftEntries, rightEntries) {
  var seen = [];
  var all = leftEntries.concat(rightEntries);
  for (var i = 0; i < all.length; i++) {
    if (seen.indexOf(all[i].cue) !== -1) {
      fail("each cue object may only be used once across both tracks");
    }
    seen.push(all[i].cue);
  }
}

function readOptions(options) {
  var result = {
    gapPenalty: DEFAULT_GAP_PENALTY,
    mergePenalty: DEFAULT_MERGE_PENALTY,
    locks: []
  };
  if (options === undefined || options === null) {
    return result;
  }
  if (typeof options !== "object" || Array.isArray(options)) {
    fail("options must be an object");
  }
  ["gapPenalty", "mergePenalty"].forEach(function(name) {
    if (options[name] !== undefined) {
      var value = options[name];
      if (!isInteger(value) || value < 0 || value > MAX_PENALTY) {
        fail("options." + name + " must be an integer in [0, " +
             MAX_PENALTY + "]");
      }
      result[name] = value;
    }
  });
  if (options.locks !== undefined) {
    if (!Array.isArray(options.locks)) {
      fail("options.locks must be an array");
    }
    result.locks = options.locks;
  }
  return result;
}

function keyIndexMap(entries) {
  var map = Object.create(null);
  for (var i = 0; i < entries.length; i++) {
    map[entries[i].key] = i;
  }
  return map;
}

// Validate one side of a lock: an array of existing keys that references
// consecutive input entries in track order. Returns the entry indices.
function lockSideIndices(keys, side, index, k) {
  if (!Array.isArray(keys)) {
    fail("locks[" + k + "]." + side + " must be an array of keys");
  }
  var result = [];
  for (var i = 0; i < keys.length; i++) {
    var key = keys[i];
    if (typeof key !== "string" || key === "" || index[key] === undefined) {
      fail("locks[" + k + "]." + side + " references unknown key " +
           JSON.stringify(key));
    }
    result.push(index[key]);
  }
  for (i = 1; i < result.length; i++) {
    if (result[i] !== result[i - 1] + 1) {
      fail("locks[" + k + "]." + side +
           " keys must be consecutive and in track order");
    }
  }
  return result;
}

// Validate the lock list against the two tracks and return index ranges
// [{ls, le, rs, re}] (end exclusive) in consumption order. Locks must be
// 1:1, 1:2 or 2:1 blocks, strictly increasing and non-overlapping on both
// tracks; anything else is rejected, never silently reordered.
function lockRanges(locks, leftEntries, rightEntries) {
  var leftIndex = keyIndexMap(leftEntries);
  var rightIndex = keyIndexMap(rightEntries);
  var ranges = [];
  var prevLe = 0;
  var prevRe = 0;
  for (var k = 0; k < locks.length; k++) {
    var lock = locks[k];
    if (!lock || typeof lock !== "object" || Array.isArray(lock)) {
      fail("locks[" + k + "] must be an object with left and right keys");
    }
    var li = lockSideIndices(lock.left, "left", leftIndex, k);
    var ri = lockSideIndices(lock.right, "right", rightIndex, k);
    var shape = li.length + ":" + ri.length;
    if (shape !== "1:1" && shape !== "1:2" && shape !== "2:1") {
      fail("locks[" + k + "] must reference one or two cues per side " +
           "(1:1, 1:2 or 2:1)");
    }
    var range = {
      ls: li[0],
      le: li[li.length - 1] + 1,
      rs: ri[0],
      re: ri[ri.length - 1] + 1
    };
    if (range.ls < prevLe || range.rs < prevRe) {
      fail("locks[" + k + "] crosses, overlaps or is out of order with " +
           "an earlier lock");
    }
    prevLe = range.le;
    prevRe = range.re;
    ranges.push(range);
  }
  return ranges;
}

// Normalized deep copies of validated locks (the caller's objects and the
// model's own state stay independent).
function normalizeLocks(locks) {
  return locks.map(function(lock) {
    return {left: lock.left.slice(), right: lock.right.slice()};
  });
}

// Cost of matching left entries [li, li+nl) against right entries
// [ri, ri+nr): envelope start/end deltas plus the merge penalty.
function matchCost(leftTimes, li, nl, rightTimes, ri, nr, mergePenalty) {
  var ls = Infinity;
  var le = -Infinity;
  var rs = Infinity;
  var re = -Infinity;
  for (var i = 0; i < nl; i++) {
    ls = Math.min(ls, leftTimes[li + i].start);
    le = Math.max(le, leftTimes[li + i].end);
  }
  for (var j = 0; j < nr; j++) {
    rs = Math.min(rs, rightTimes[ri + j].start);
    re = Math.max(re, rightTimes[ri + j].end);
  }
  return Math.abs(ls - rs) + Math.abs(le - re) +
         mergePenalty * (nl + nr - 2);
}

// Solve one lock-free segment for the minimum total cost with a dynamic
// program over the whole segment (never a per-cue greedy choice). Ties are
// broken by the lexicographic order of the full transition sequence with
// transition priority 11 < 12 < 21 < 10 < 01. Returns a list of [nl, nr]
// transitions covering left[li, lj) and right[ri, rj).
function solveSegment(leftTimes, rightTimes, li, lj, ri, rj,
                      gapPenalty, mergePenalty) {
  var n = lj - li;
  var m = rj - ri;
  var cost = [];
  var i;
  var j;
  for (i = 0; i <= n; i++) {
    cost.push(new Array(m + 1));
  }
  cost[n][m] = 0;
  for (i = n; i >= 0; i--) {
    for (j = m; j >= 0; j--) {
      if (i === n && j === m) {
        continue;
      }
      var best = Infinity;
      if (i < n && j < m) {
        best = Math.min(best,
          matchCost(leftTimes, li + i, 1, rightTimes, ri + j, 1,
                    mergePenalty) + cost[i + 1][j + 1]);
      }
      if (i < n && j + 2 <= m) {
        best = Math.min(best,
          matchCost(leftTimes, li + i, 1, rightTimes, ri + j, 2,
                    mergePenalty) + cost[i + 1][j + 2]);
      }
      if (i + 2 <= n && j < m) {
        best = Math.min(best,
          matchCost(leftTimes, li + i, 2, rightTimes, ri + j, 1,
                    mergePenalty) + cost[i + 2][j + 1]);
      }
      if (i < n) {
        best = Math.min(best, gapPenalty + cost[i + 1][j]);
      }
      if (j < m) {
        best = Math.min(best, gapPenalty + cost[i][j + 1]);
      }
      cost[i][j] = best;
    }
  }
  // Reconstruct greedily in transition-priority order: the first
  // transition that still reaches the optimum is the lexicographically
  // smallest choice, and the same holds recursively for the remainder.
  var transitions = [];
  i = 0;
  j = 0;
  while (i < n || j < m) {
    var current = cost[i][j];
    if (i < n && j < m &&
        matchCost(leftTimes, li + i, 1, rightTimes, ri + j, 1,
                  mergePenalty) + cost[i + 1][j + 1] === current) {
      transitions.push([1, 1]);
      i += 1;
      j += 1;
    } else if (i < n && j + 2 <= m &&
               matchCost(leftTimes, li + i, 1, rightTimes, ri + j, 2,
                         mergePenalty) + cost[i + 1][j + 2] === current) {
      transitions.push([1, 2]);
      i += 1;
      j += 2;
    } else if (i + 2 <= n && j < m &&
               matchCost(leftTimes, li + i, 2, rightTimes, ri + j, 1,
                         mergePenalty) + cost[i + 2][j + 1] === current) {
      transitions.push([2, 1]);
      i += 2;
      j += 1;
    } else if (i < n && gapPenalty + cost[i + 1][j] === current) {
      transitions.push([1, 0]);
      i += 1;
    } else if (j < m && gapPenalty + cost[i][j + 1] === current) {
      transitions.push([0, 1]);
      j += 1;
    } else {
      fail("internal error: no transition reproduces the optimal cost");
    }
  }
  return transitions;
}

// Solve the whole tracks: locked blocks are fixed single transitions and
// the segments before, between and after them are solved independently.
function solve(leftTimes, rightTimes, ranges, gapPenalty, mergePenalty) {
  var transitions = [];
  var li = 0;
  var ri = 0;
  for (var k = 0; k <= ranges.length; k++) {
    var le = k < ranges.length ? ranges[k].ls : leftTimes.length;
    var re = k < ranges.length ? ranges[k].rs : rightTimes.length;
    var segment = solveSegment(leftTimes, rightTimes, li, le, ri, re,
                               gapPenalty, mergePenalty);
    for (var s = 0; s < segment.length; s++) {
      transitions.push({nl: segment[s][0], nr: segment[s][1], locked: false});
    }
    if (k < ranges.length) {
      transitions.push({
        nl: ranges[k].le - ranges[k].ls,
        nr: ranges[k].re - ranges[k].rs,
        locked: true
      });
      li = ranges[k].le;
      ri = ranges[k].re;
    }
  }
  return transitions;
}

function transitionsToGroups(transitions, leftEntries, rightEntries) {
  var groups = [];
  var li = 0;
  var ri = 0;
  for (var t = 0; t < transitions.length; t++) {
    var tr = transitions[t];
    var leftKeys = [];
    var rightKeys = [];
    for (var a = 0; a < tr.nl; a++) {
      leftKeys.push(leftEntries[li++].key);
    }
    for (var b = 0; b < tr.nr; b++) {
      rightKeys.push(rightEntries[ri++].key);
    }
    groups.push({left: leftKeys, right: rightKeys, locked: tr.locked});
  }
  return groups;
}

function BilingualAlignment(leftEntries, rightEntries, gapPenalty,
                            mergePenalty, locks, groups) {
  this._left = leftEntries;
  this._right = rightEntries;
  this._gapPenalty = gapPenalty;
  this._mergePenalty = mergePenalty;
  this._locks = locks;
  this._groups = groups;
}

// Associate two tracks of {key, cue} entries. Throws on invalid input;
// never modifies the given arrays, cues or options.
BilingualAlignment.align = function(left, right, options) {
  var opts = readOptions(options);
  var leftEntries = validateTrackInput(left, "left");
  var rightEntries = validateTrackInput(right, "right");
  assertDistinctCues(leftEntries, rightEntries);
  var leftTimes = readTrackTimes(leftEntries, "left");
  var rightTimes = readTrackTimes(rightEntries, "right");
  var ranges = lockRanges(opts.locks, leftEntries, rightEntries);
  var transitions = solve(leftTimes, rightTimes, ranges,
                          opts.gapPenalty, opts.mergePenalty);
  var groups = transitionsToGroups(transitions, leftEntries, rightEntries);
  return new BilingualAlignment(leftEntries, rightEntries,
                                opts.gapPenalty, opts.mergePenalty,
                                normalizeLocks(opts.locks), groups);
};

Object.defineProperties(BilingualAlignment.prototype, {
  // The current association as {left: [key], right: [key], locked}
  // groups in consumption order. A fresh copy is returned every time;
  // direct modification of the model's groups is not supported.
  groups: {
    get: function() {
      return this._groups.map(function(group) {
        return {
          left: group.left.slice(),
          right: group.right.slice(),
          locked: group.locked
        };
      });
    }
  },
  // The locks this model was built with, as {left: [key], right: [key]}.
  locks: {
    get: function() {
      return this._locks.map(function(lock) {
        return {left: lock.left.slice(), right: lock.right.slice()};
      });
    }
  },
  // The held {key, cue} entries of each track, in input order.
  left: {
    get: function() {
      return this._left.map(function(entry) {
        return {key: entry.key, cue: entry.cue};
      });
    }
  },
  right: {
    get: function() {
      return this._right.map(function(entry) {
        return {key: entry.key, cue: entry.cue};
      });
    }
  },
  // Total cost of the current groups under the current cue times.
  // Recomputed on every read; never trusts persisted values.
  cost: {
    get: function() {
      var leftIndex = keyIndexMap(this._left);
      var rightIndex = keyIndexMap(this._right);
      var total = 0;
      for (var g = 0; g < this._groups.length; g++) {
        var group = this._groups[g];
        var nl = group.left.length;
        var nr = group.right.length;
        if (nl === 0 || nr === 0) {
          total += this._gapPenalty * (nl + nr);
          continue;
        }
        var leftTimes = [];
        var rightTimes = [];
        for (var a = 0; a < nl; a++) {
          var lcue = this._left[leftIndex[group.left[a]]].cue;
          leftTimes.push({
            start: Math.round(lcue.startTime * 1000),
            end: Math.round(lcue.endTime * 1000)
          });
        }
        for (var b = 0; b < nr; b++) {
          var rcue = this._right[rightIndex[group.right[b]]].cue;
          rightTimes.push({
            start: Math.round(rcue.startTime * 1000),
            end: Math.round(rcue.endTime * 1000)
          });
        }
        total += matchCost(leftTimes, 0, nl, rightTimes, 0, nr,
                           this._mergePenalty);
      }
      return total;
    }
  }
});

// Re-solve the association from the current cue times, keeping the manual
// locks (by key). On failure the current groups are kept untouched.
BilingualAlignment.prototype.realign = function() {
  var leftTimes = readTrackTimes(this._left, "left");
  var rightTimes = readTrackTimes(this._right, "right");
  var ranges = lockRanges(this._locks, this._left, this._right);
  var transitions = solve(leftTimes, rightTimes, ranges,
                          this._gapPenalty, this._mergePenalty);
  this._groups = transitionsToGroups(transitions, this._left, this._right);
  return this;
};

// Conform the right cues of a matched group to the group's left envelope:
// every right endpoint x maps to L0 + floor((x - R0) * (L1 - L0) /
// (R1 - R0)) in exact integer milliseconds. With ripple=true every
// right-track entry after the group's last right entry (including skipped
// ones) is shifted by the end-point delta (L1 - R1). The whole edit is
// computed and validated before any cue is written back through its
// native setters; on failure nothing changes.
BilingualAlignment.prototype.conform = function(groupIndex, ripple) {
  if (ripple === undefined) {
    ripple = false;
  }
  if (!isInteger(groupIndex)) {
    fail("groupIndex must be an integer");
  }
  if (typeof ripple !== "boolean") {
    fail("ripple must be a boolean");
  }
  if (groupIndex < 0 || groupIndex >= this._groups.length) {
    fail("groupIndex " + groupIndex + " is out of range");
  }
  var group = this._groups[groupIndex];
  if (group.left.length === 0 || group.right.length === 0) {
    fail("group " + groupIndex + " does not reference both tracks");
  }
  // The whole model must currently satisfy the input contract.
  var leftTimes = readTrackTimes(this._left, "left");
  var rightTimes = readTrackTimes(this._right, "right");
  var leftIndex = keyIndexMap(this._left);
  var rightIndex = keyIndexMap(this._right);

  // Envelopes of the selected group.
  var L0 = Infinity;
  var L1 = -Infinity;
  var R0 = Infinity;
  var R1 = -Infinity;
  group.left.forEach(function(key) {
    var t = leftTimes[leftIndex[key]];
    L0 = Math.min(L0, t.start);
    L1 = Math.max(L1, t.end);
  });
  group.right.forEach(function(key) {
    var t = rightTimes[rightIndex[key]];
    R0 = Math.min(R0, t.start);
    R1 = Math.max(R1, t.end);
  });

  // Plan every new time up front, from the original endpoints, in exact
  // integer arithmetic (the products can exceed 2^53).
  var numerator = BigInt(L1 - L0);
  var denominator = BigInt(R1 - R0);
  function mapEndpoint(x) {
    var product = BigInt(x - R0) * numerator;
    return L0 + Number(product / denominator);
  }
  var affected = Object.create(null); // right track index -> {start, end}
  group.right.forEach(function(key) {
    var idx = rightIndex[key];
    var t = rightTimes[idx];
    affected[idx] = {start: mapEndpoint(t.start), end: mapEndpoint(t.end)};
  });
  if (ripple) {
    var lastIdx = rightIndex[group.right[group.right.length - 1]];
    var shift = L1 - R1;
    for (var i = lastIdx + 1; i < this._right.length; i++) {
      affected[i] = {
        start: rightTimes[i].start + shift,
        end: rightTimes[i].end + shift
      };
    }
  }
  var indices = Object.keys(affected).map(Number).sort(function(a, b) {
    return a - b;
  });

  // Cues carrying inline timestamp tags must not be retimed.
  for (var a = 0; a < indices.length; a++) {
    var entry = this._right[indices[a]];
    if (INLINE_TIMESTAMP.test(String(entry.cue.text))) {
      fail("cue \"" + entry.key + "\" contains an inline timestamp tag");
    }
  }
  // Every planned time must stay in range with a positive duration.
  for (var b = 0; b < indices.length; b++) {
    var plan = affected[indices[b]];
    if (plan.start < 0 || plan.start > MAX_TIME_MS ||
        plan.end < 0 || plan.end > MAX_TIME_MS) {
      fail("conform would move a cue outside the [0, " + MAX_TIME_MS +
           "] ms range");
    }
    if (plan.end <= plan.start) {
      fail("conform would collapse cue \"" +
           this._right[indices[b]].key + "\" to a non-positive duration");
    }
  }
  // The right track start order must survive the edit.
  var prevStart = -1;
  for (var c = 0; c < this._right.length; c++) {
    var start = affected[c] ? affected[c].start : rightTimes[c].start;
    if (start < prevStart) {
      fail("conform would break the right track start order");
    }
    prevStart = start;
  }

  // Everything checked out: write back through the native setters so
  // hasBeenReset is set and any existing displayState is invalidated by
  // the processing model as usual. Unaffected cues are not touched.
  for (var d = 0; d < indices.length; d++) {
    var cue = this._right[indices[d]].cue;
    var times = affected[indices[d]];
    cue.startTime = times.start / 1000;
    cue.endTime = times.end / 1000;
  }
  return this;
};

function serializeRegion(region) {
  var result = {};
  REGION_FIELDS.forEach(function(field) {
    result[field] = region[field];
  });
  return result;
}

// Serialize the model into a versioned, JSON.stringify-able document.
BilingualAlignment.prototype.toJSON = function() {
  var regions = [];
  var regionObjects = [];
  function regionRef(region) {
    if (region === null || region === undefined) {
      return null;
    }
    if (!(region instanceof VTTRegion)) {
      fail("cue region must be a VTTRegion");
    }
    // Identity-based table: a shared region keeps one entry, distinct
    // regions are never merged by property value.
    var idx = regionObjects.indexOf(region);
    if (idx === -1) {
      idx = regions.length;
      regions.push(serializeRegion(region));
      regionObjects.push(region);
    }
    return idx;
  }
  function serializeEntry(entry) {
    var cue = entry.cue;
    return {
      key: entry.key,
      cue: {
        id: cue.id,
        startTime: cue.startTime,
        endTime: cue.endTime,
        text: cue.text,
        pauseOnExit: cue.pauseOnExit,
        vertical: cue.vertical,
        snapToLines: cue.snapToLines,
        line: cue.line,
        lineAlign: cue.lineAlign,
        position: cue.position,
        positionAlign: cue.positionAlign,
        size: cue.size,
        align: cue.align,
        region: regionRef(cue.region)
      }
    };
  }
  var left = this._left.map(serializeEntry);
  var right = this._right.map(serializeEntry);
  return {
    version: FORMAT_VERSION,
    options: {
      gapPenalty: this._gapPenalty,
      mergePenalty: this._mergePenalty
    },
    regions: regions,
    left: left,
    right: right,
    locks: this.locks,
    groups: this.groups
  };
};

function deserializeRegion(value, index) {
  if (!value || typeof value !== "object" || Array.isArray(value)) {
    fail("regions[" + index + "] must be an object");
  }
  var region = new VTTRegion();
  REGION_FIELDS.forEach(function(field) {
    if (value[field] === undefined) {
      fail("regions[" + index + "] is missing \"" + field + "\"");
    }
    // Native setters validate the values.
    region[field] = value[field];
  });
  return region;
}

function deserializeCue(value, where, regions) {
  if (!value || typeof value !== "object" || Array.isArray(value)) {
    fail(where + " cue must be an object");
  }
  CUE_FIELDS.forEach(function(field) {
    if (value[field] === undefined) {
      fail(where + " cue is missing \"" + field + "\"");
    }
  });
  if (typeof value.id !== "string" || typeof value.text !== "string") {
    fail(where + " cue id and text must be strings");
  }
  if (typeof value.pauseOnExit !== "boolean" ||
      typeof value.snapToLines !== "boolean") {
    fail(where + " cue pauseOnExit and snapToLines must be booleans");
  }
  if (typeof value.vertical !== "string" ||
      typeof value.lineAlign !== "string" ||
      typeof value.positionAlign !== "string" ||
      typeof value.align !== "string") {
    fail(where + " cue layout settings must be strings");
  }
  if (typeof value.size !== "number") {
    fail(where + " cue size must be a number");
  }
  if (!(typeof value.line === "number" || value.line === "auto") ||
      !(typeof value.position === "number" || value.position === "auto")) {
    fail(where + " cue line and position must be numbers or \"auto\"");
  }
  var region = null;
  if (value.region !== null) {
    if (!isInteger(value.region) || value.region < 0 ||
        value.region >= regions.length) {
      fail(where + " cue has a bad region reference " +
           JSON.stringify(value.region));
    }
    region = regions[value.region];
  }
  // Rebuild through the native constructor and setters so every value is
  // validated the same way as a parsed cue.
  var cue = new VTTCue(value.startTime, value.endTime, value.text);
  cue.id = value.id;
  cue.pauseOnExit = value.pauseOnExit;
  cue.region = region;
  cue.vertical = value.vertical;
  cue.snapToLines = value.snapToLines;
  cue.line = value.line;
  cue.lineAlign = value.lineAlign;
  cue.position = value.position;
  cue.positionAlign = value.positionAlign;
  cue.size = value.size;
  cue.align = value.align;
  // Cache state is not persisted: restored cues start clean.
  cue.hasBeenReset = false;
  cue.displayState = undefined;
  return cue;
}

function deserializeTrack(value, where, regions) {
  if (!Array.isArray(value)) {
    fail(where + " must be an array");
  }
  if (value.length > MAX_ENTRIES) {
    fail(where + " must not have more than " + MAX_ENTRIES + " entries");
  }
  var seenKeys = Object.create(null);
  var entries = [];
  for (var i = 0; i < value.length; i++) {
    var item = value[i];
    if (!item || typeof item !== "object" || Array.isArray(item)) {
      fail(where + "[" + i + "] must be an object with key and cue");
    }
    var key = item.key;
    if (typeof key !== "string" || key === "") {
      fail(where + "[" + i + "] must have a non-empty string key");
    }
    if (seenKeys[key]) {
      fail(where + "[" + i + "] duplicates key \"" + key + "\"");
    }
    seenKeys[key] = true;
    entries.push({
      key: key,
      cue: deserializeCue(item.cue, where + "[" + i + "]", regions)
    });
  }
  return entries;
}

function keysEqual(a, b) {
  if (a.length !== b.length) {
    return false;
  }
  for (var i = 0; i < a.length; i++) {
    if (a[i] !== b[i]) {
      return false;
    }
  }
  return true;
}

// Validate that the persisted groups form a complete legal consumption of
// both tracks and that the locked groups match the persisted locks.
// Optimality of the path is not required (conform does not realign).
function validateGroups(value, locks, leftEntries, rightEntries) {
  if (!Array.isArray(value)) {
    fail("groups must be an array");
  }
  var li = 0;
  var ri = 0;
  var lockCursor = 0;
  var groups = [];
  for (var g = 0; g < value.length; g++) {
    var group = value[g];
    if (!group || typeof group !== "object" || Array.isArray(group)) {
      fail("groups[" + g + "] must be an object");
    }
    if (!Array.isArray(group.left) || !Array.isArray(group.right) ||
        typeof group.locked !== "boolean") {
      fail("groups[" + g + "] must have key arrays and a boolean locked");
    }
    var nl = group.left.length;
    var nr = group.right.length;
    var shape = nl + ":" + nr;
    if (shape !== "1:1" && shape !== "1:2" && shape !== "2:1" &&
        shape !== "1:0" && shape !== "0:1") {
      fail("groups[" + g + "] has an unsupported shape " + shape);
    }
    var leftKeys = [];
    var rightKeys = [];
    for (var a = 0; a < nl; a++) {
      if (li >= leftEntries.length || leftEntries[li].key !== group.left[a]) {
        fail("groups[" + g + "] does not consume the left track in order");
      }
      leftKeys.push(group.left[a]);
      li++;
    }
    for (var b = 0; b < nr; b++) {
      if (ri >= rightEntries.length ||
          rightEntries[ri].key !== group.right[b]) {
        fail("groups[" + g + "] does not consume the right track in order");
      }
      rightKeys.push(group.right[b]);
      ri++;
    }
    if (group.locked) {
      var lock = locks[lockCursor];
      if (!lock || !keysEqual(lock.left, group.left) ||
          !keysEqual(lock.right, group.right)) {
        fail("groups[" + g + "]: locked group does not match locks[" +
             lockCursor + "]");
      }
      lockCursor++;
    }
    groups.push({left: leftKeys, right: rightKeys, locked: group.locked});
  }
  if (li !== leftEntries.length || ri !== rightEntries.length) {
    fail("groups do not form a complete consumption of both tracks");
  }
  if (lockCursor !== locks.length) {
    fail("not every lock appears as a locked group");
  }
  return groups;
}

// Restore a model from a JSON.parse'd toJSON() document. Validates the
// version, keys, times, locks, the complete group path and the region
// references; on any failure nothing is returned.
BilingualAlignment.fromJSON = function(value) {
  if (!value || typeof value !== "object" || Array.isArray(value)) {
    fail("fromJSON expects an object produced by toJSON");
  }
  if (value.version !== FORMAT_VERSION) {
    fail("unsupported document version " + JSON.stringify(value.version));
  }
  ["options", "regions", "left", "right", "locks", "groups"].forEach(
    function(field) {
      if (value[field] === undefined) {
        fail("document is missing field \"" + field + "\"");
      }
    });
  var opts = readOptions(value.options);
  if (!Array.isArray(value.regions)) {
    fail("regions must be an array");
  }
  if (!Array.isArray(value.locks)) {
    fail("locks must be an array");
  }
  var regions = value.regions.map(deserializeRegion);
  var leftEntries = deserializeTrack(value.left, "left", regions);
  var rightEntries = deserializeTrack(value.right, "right", regions);
  readTrackTimes(leftEntries, "left");
  readTrackTimes(rightEntries, "right");
  lockRanges(value.locks, leftEntries, rightEntries);
  var groups = validateGroups(value.groups, value.locks,
                              leftEntries, rightEntries);
  return new BilingualAlignment(leftEntries, rightEntries,
                                opts.gapPenalty, opts.mergePenalty,
                                normalizeLocks(value.locks), groups);
};

module.exports = BilingualAlignment;
