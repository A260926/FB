'use strict';
// MPE output mode of JZZ.M2M1(): byte-level black-box tests.
// Run with: node --test test/m2m1-mpe.test.js
const test = require('node:test');
const assert = require('node:assert/strict');
const JZZ = require('../javascript/JZZ.js');

function recorder() {
  var log = [];
  return {
    log: log,
    fn: function(msg) { log.push({ bytes: Array.from(msg), gr: msg.gr }); }
  };
}
function bytes(log) { return log.map(function(x) { return x.bytes; }); }
function ump(arr) { return JZZ.UMP(arr); }
// type 4 UMP builders
function noteOn(g, c, n, v) { return ump([0x40 + g, 0x90 + c, n, 0, (v >> 8) & 255, v & 255, 0, 0]); }
function noteOff(g, c, n, v) { return ump([0x40 + g, 0x80 + c, n, 0, (v >> 8) & 255, v & 255, 0, 0]); }
function pnBend(g, c, n, v32) {
  return ump([0x40 + g, 0x60 + c, n, 0, (v32 >> 24) & 255, (v32 >> 16) & 255, (v32 >> 8) & 255, v32 & 255]);
}
function pnPressure(g, c, n, v32) {
  return ump([0x40 + g, 0xa0 + c, n, 0, (v32 >> 24) & 255, (v32 >> 16) & 255, (v32 >> 8) & 255, v32 & 255]);
}
function chBend(g, c, v32) {
  return ump([0x40 + g, 0xe0 + c, 0, 0, (v32 >> 24) & 255, (v32 >> 16) & 255, (v32 >> 8) & 255, v32 & 255]);
}
function cc(g, c, num, v32) {
  return ump([0x40 + g, 0xb0 + c, num, 0, (v32 >> 24) & 255, (v32 >> 16) & 255, (v32 >> 8) & 255, v32 & 255]);
}
function rpnSetup(ch, range) {
  return [
    [0xb0 + ch, 101, 0], [0xb0 + ch, 100, 0],
    [0xb0 + ch, 6, range], [0xb0 + ch, 38, 0],
    [0xb0 + ch, 101, 127], [0xb0 + ch, 100, 127]
  ];
}

test('configuration is validated at construction time', function() {
  var bad = [
    { master: 16, members: [1], bendRange: 48 },
    { master: 1.5, members: [1], bendRange: 48 },
    { master: 0, members: [], bendRange: 48 },
    { master: 0, members: [1, 1], bendRange: 48 },
    { master: 0, members: [0], bendRange: 48 },
    { master: 0, members: [16], bendRange: 48 },
    { master: 0, members: [1], bendRange: 0 },
    { master: 0, members: [1], bendRange: 97 },
    { master: 0, members: [1], bendRange: 48.5 },
    { master: 0, bendRange: 48 },
    { members: [1], bendRange: 48 }
  ];
  bad.forEach(function(opt) {
    assert.throws(function() { JZZ.M2M1({ mpe: opt }); });
  });
  // a rejected constructor never sends anything
  var rec = recorder();
  var p = JZZ.M2M1();
  p.connect(rec.fn);
  assert.throws(function() { JZZ.M2M1({ mpe: { master: -1, members: [1], bendRange: 48 } }); });
  assert.equal(rec.log.length, 0);
  // the valid shape works
  assert.doesNotThrow(function() { JZZ.M2M1({ mpe: { master: 0, members: [1, 2], bendRange: 48 } }); });
  // boundary values are accepted
  assert.doesNotThrow(function() { JZZ.M2M1({ mpe: { master: 15, members: [0, 14], bendRange: 1 } }); });
  assert.doesNotThrow(function() { JZZ.M2M1({ mpe: { master: 0, members: [15], bendRange: 96 } }); });
  // an empty options object behaves like no arguments
  var plain = JZZ.M2M1({});
  assert.equal(plain._orig._mpeBridge, undefined);
});

test('same note from two groups and two source channels gets independent members', function() {
  var rec = recorder();
  var p = JZZ.M2M1({ mpe: { master: 0, members: [1, 2, 3], bendRange: 48 } });
  p.connect(rec.fn);
  p.send(noteOn(0, 5, 60, 0xffff));
  p.send(noteOn(1, 5, 60, 0xffff));
  p.send(noteOn(0, 6, 60, 0xffff));
  p.send(pnBend(0, 5, 60, 0xc0000000));
  p.send(pnBend(1, 5, 60, 0xc0000000));
  p.send(pnBend(0, 6, 60, 0xc0000000));
  p.send(noteOff(0, 5, 60, 0));
  p.send(noteOff(1, 5, 60, 0));
  p.send(noteOff(0, 6, 60, 0));
  var out = bytes(rec.log);
  // each source got its own member channel, carrying the source group
  assert.deepEqual(out.filter(function(m) { return m[0] == 0x91; }), [[0x91, 60, 127]]);
  assert.deepEqual(out.filter(function(m) { return m[0] == 0x92; }), [[0x92, 60, 127]]);
  assert.deepEqual(out.filter(function(m) { return m[0] == 0x93; }), [[0x93, 60, 127]]);
  // preflight wheels at center, then one independent +24-semitone wheel per member
  assert.deepEqual(out.filter(function(m) { return (m[0] & 0xf0) == 0xe0; }),
    [[0xe1, 0, 0x40], [0xe2, 0, 0x40], [0xe3, 0, 0x40],
     [0xe1, 0, 0x60], [0xe2, 0, 0x60], [0xe3, 0, 0x60]]);
  assert.deepEqual(out.filter(function(m) { return (m[0] & 0xf0) == 0x80; }),
    [[0x81, 60, 0], [0x82, 60, 0], [0x83, 60, 0]]);
  // everything emitted for the group 1 note stays on member channel 2
  var g1 = rec.log.filter(function(x) { return x.gr == 1; }).map(function(x) { return x.bytes[0] & 15; });
  assert.ok(g1.length > 0);
  assert.deepEqual(Array.from(new Set(g1)), [2]);
});

test('channel and per-note wheels are combined, rounded and clamped to the member range', function() {
  var rec = recorder();
  var p = JZZ.M2M1({ mpe: { master: 0, members: [1], bendRange: 48 } });
  p.connect(rec.fn);
  p.send(noteOn(0, 5, 60, 0xffff));
  var wheels = [];
  function wheel() {
    var m = rec.log[rec.log.length - 1].bytes;
    assert.equal(m[0] & 0xf0, 0xe0);
    wheels.push([m[1], m[2]]);
  }
  p.send(pnBend(0, 5, 60, 0xffffffff)); wheel(); // +48 semitones: top endpoint
  p.send(pnBend(0, 5, 60, 0x00000000)); wheel(); // -48 semitones: bottom endpoint
  p.send(pnBend(0, 5, 60, 0x80000000)); wheel(); // center
  p.send(chBend(0, 5, 0xffffffff)); wheel();      // +2 semitones channel wheel only
  p.send(pnBend(0, 5, 60, 0xc0000000)); wheel();  // +24 per-note on top of +2 = +26
  p.send(chBend(0, 5, 0xffffffff)); wheel();      // channel change keeps the per-note offset
  p.send(pnBend(0, 5, 60, 0xffffffff)); wheel();  // +50 total, clamped to +48
  p.send(chBend(0, 5, 0x00000000)); wheel();      // channel -2, per-note +48: +46
  p.send(pnBend(0, 5, 60, 0x00000000)); wheel();  // -50 total, clamped to -48
  p.send(chBend(0, 5, 0x80000000));
  p.send(pnBend(0, 5, 60, 0x80000000)); wheel();  // back to center 8192
  assert.deepEqual(wheels, [
    [0x7f, 0x7f], [0x00, 0x00], [0x00, 0x40],
    [0x55, 0x42], [0x55, 0x62], [0x55, 0x62],
    [0x7f, 0x7f], [0x2b, 0x7d], [0x00, 0x00], [0x00, 0x40]
  ]);
});

test('pool exhaustion preempts the oldest note and late releases cannot touch the new note', function() {
  var rec = recorder();
  var p = JZZ.M2M1({ mpe: { master: 0, members: [1, 2], bendRange: 48 } });
  p.connect(rec.fn);
  p.send(noteOn(0, 5, 60, 0xffff)); // A -> member 1
  p.send(noteOn(0, 6, 62, 0xffff)); // B -> member 2
  rec.log.length = 0;
  p.send(noteOn(0, 7, 64, 0xffff)); // C steals A: NoteOff, reset, preflight, NoteOn
  assert.deepEqual(bytes(rec.log), [
    [0x81, 60, 0],          // stolen NoteOff first
    [0xb1, 121, 0],         // reset all controllers on the reused member
    [0xb1, 1, 0], [0xb1, 11, 127], [0xd1, 0], [0xe1, 0, 0x40],
    [0x91, 64, 127]
  ]);
  rec.log.length = 0;
  p.send(noteOff(0, 5, 60, 0));       // late NoteOff of the stolen note: ignored
  p.send(pnBend(0, 5, 60, 0xffffffff)); // late per-note bend: ignored
  p.send(pnPressure(0, 5, 60, 0xffffffff)); // late per-note pressure: ignored
  assert.equal(rec.log.length, 0);
  p.send(noteOff(0, 6, 62, 0));       // B releases member 2
  p.send(noteOff(0, 7, 64, 0));       // C releases member 1
  assert.deepEqual(bytes(rec.log), [[0x82, 62, 0], [0x81, 64, 0]]);
  // the freed pool reuses the first free member without another RPN block
  rec.log.length = 0;
  p.send(noteOn(0, 8, 66, 0xffff));
  assert.deepEqual(bytes(rec.log), [
    [0xb1, 1, 0], [0xb1, 11, 127], [0xd1, 0], [0xe1, 0, 0x40], [0x91, 66, 127]
  ]);
});

test('retriggering the same group/channel/note ends the old sound before the new attack', function() {
  var rec = recorder();
  var p = JZZ.M2M1({ mpe: { master: 0, members: [1], bendRange: 48 } });
  p.connect(rec.fn);
  p.send(noteOn(0, 5, 60, 0xffff));
  rec.log.length = 0;
  p.send(noteOn(0, 5, 60, 0x8000)); // same triplet
  assert.deepEqual(bytes(rec.log), [
    [0x81, 60, 0],                                     // old note ends
    [0xb1, 1, 0], [0xb1, 11, 127], [0xd1, 0], [0xe1, 0, 0x40],
    [0x91, 60, 64]                                     // fresh attack, 16->7 bit velocity
  ]);
  p.send(noteOff(0, 5, 60, 0));
  assert.deepEqual(bytes(rec.log).slice(-1), [[0x81, 60, 0]]);
  // status 9 with zero velocity is also a note off
  rec.log.length = 0;
  p.send(noteOn(0, 5, 60, 0xffff));
  p.send(noteOn(0, 5, 60, 0x0000));
  assert.deepEqual(bytes(rec.log).slice(-1), [[0x81, 60, 0]]);
});

test('CC1/CC11 are cached while idle, inherited by new notes and fanned out to active members', function() {
  var rec = recorder();
  var p = JZZ.M2M1({ mpe: { master: 0, members: [1, 2], bendRange: 48 } });
  p.connect(rec.fn);
  p.send(cc(0, 5, 1, 0xc0000000));  // cached as 96, no active note: nothing emitted
  p.send(cc(0, 5, 11, 0x40000000)); // cached as 32
  assert.equal(rec.log.length, 0);
  p.send(noteOn(0, 5, 60, 0xffff));
  assert.deepEqual(bytes(rec.log), rpnSetup(1, 48).concat([
    [0xb1, 1, 96], [0xb1, 11, 32], [0xd1, 0], [0xe1, 0, 0x40], [0x91, 60, 127]
  ]));
  p.send(noteOn(0, 5, 62, 0xffff)); // second active member
  rec.log.length = 0;
  p.send(cc(0, 5, 1, 0xffffffff)); // 127, reaches all active members
  assert.deepEqual(bytes(rec.log), [[0xb1, 1, 127], [0xb2, 1, 127]]);
  p.send(noteOff(0, 5, 60, 0));
  rec.log.length = 0;
  p.send(noteOn(0, 5, 64, 0xffff)); // new note takes the cached value, no new RPN
  assert.deepEqual(bytes(rec.log), [
    [0xb1, 1, 127], [0xb1, 11, 32], [0xd1, 0], [0xe1, 0, 0x40], [0x91, 64, 127]
  ]);
});

test('per-note pressure becomes channel pressure and is dropped without an active note', function() {
  var rec = recorder();
  var p = JZZ.M2M1({ mpe: { master: 0, members: [1], bendRange: 48 } });
  p.connect(rec.fn);
  p.send(pnPressure(0, 5, 60, 0xc0000000)); // no active note yet
  assert.equal(rec.log.length, 0);
  p.send(noteOn(0, 5, 60, 0xffff));
  rec.log.length = 0;
  p.send(pnPressure(0, 5, 60, 0xc0000000)); // 32-bit -> 7-bit = 96
  p.send(pnPressure(0, 5, 60, 0xffffffff)); // 127
  assert.deepEqual(bytes(rec.log), [[0xd1, 96], [0xd1, 127]]);
});

test('two receivers get equal independent copies and synchronous echoes wait for the current batch', function() {
  var p = JZZ.M2M1({ mpe: { master: 0, members: [1], bendRange: 48 } });
  var a = [], b = [];
  var echoed = false;
  p.connect(function(msg) {
    a.push(msg);
    if (!echoed && (msg[0] & 0xf0) == 0x90) {
      echoed = true;
      p.send(noteOff(0, 5, 60, 0)); // synchronously echoed back into the bridge
    }
  });
  p.connect(function(msg) { b.push(msg); });
  p.send(noteOn(0, 5, 60, 0xffff));
  assert.equal(a.length, b.length);
  for (var i = 0; i < a.length; i++) {
    assert.notEqual(a[i], b[i]);                 // no aliasing
    assert.deepEqual(Array.from(a[i]), Array.from(b[i]));
  }
  // receiver B sees the whole preset + NoteOn before the echoed NoteOff
  var bStatuses = b.map(function(m) { return m[0]; });
  var on = bStatuses.indexOf(0x91);
  var off = bStatuses.indexOf(0x81);
  assert.ok(on > -1 && off > -1 && on < off);
  assert.deepEqual(Array.from(b[off]), [0x81, 60, 0]);
  // everything ahead of the NoteOn is the one-time RPN and the per-note preset
  assert.deepEqual(b.slice(0, on).map(function(m) { return Array.from(m); }),
    rpnSetup(1, 48).concat([[0xb1, 1, 0], [0xb1, 11, 127], [0xd1, 0], [0xe1, 0, 0x40]]));
});

test('bridge instances keep independent state', function() {
  var r1 = recorder(), r2 = recorder();
  var p1 = JZZ.M2M1({ mpe: { master: 0, members: [1], bendRange: 48 } });
  var p2 = JZZ.M2M1({ mpe: { master: 0, members: [1], bendRange: 24 } });
  p1.connect(r1.fn);
  p2.connect(r2.fn);
  p1.send(noteOn(0, 5, 60, 0xffff));
  p2.send(noteOn(0, 5, 60, 0xffff));
  // each instance performs its own first-use RPN with its own range
  assert.deepEqual(bytes(r1.log).slice(0, 6), rpnSetup(1, 48));
  assert.deepEqual(bytes(r2.log).slice(0, 6), rpnSetup(1, 24));
  p1.send(pnBend(0, 5, 60, 0xffffffff));
  p2.send(pnBend(0, 5, 60, 0xffffffff));
  assert.deepEqual(bytes(r1.log).pop(), [0xe1, 0x7f, 0x7f]);
  assert.deepEqual(bytes(r2.log).pop(), [0xe1, 0x7f, 0x7f]);
});

test('unsupported type-4 messages are ignored while other conversions and MIDI-1 pass-through stay intact', function() {
  var rec = recorder();
  var p = JZZ.M2M1({ mpe: { master: 0, members: [1], bendRange: 48 } });
  p.connect(rec.fn);
  p.send(noteOn(0, 5, 60, 0xffff));
  rec.log.length = 0;
  p.send(cc(0, 5, 64, 0xffffffff));   // sustain pedal: ignored
  p.send(ump([0x40, 0x05, 60, 0, 0, 0, 0, 0])); // per-note RPN: ignored
  p.send(ump([0x40, 0xf5, 60, 3, 0, 0, 0, 0])); // per-note management: ignored
  assert.equal(rec.log.length, 0);
  // utility/system conversion still works
  p.send(ump([0x10, 0xf8, 0, 0]));
  assert.deepEqual(rec.log[rec.log.length - 1], { bytes: [0xf8, 0, 0], gr: 0 });
  // plain MIDI 1.0 input still passes through untouched
  var m1 = JZZ.MIDI.noteOn(0, 60, 100);
  p.send(m1);
  assert.deepEqual(rec.log[rec.log.length - 1].bytes, [0x90, 60, 100]);
});

test('input messages are never mutated', function() {
  var rec = recorder();
  var p = JZZ.M2M1({ mpe: { master: 0, members: [1], bendRange: 48 } });
  p.connect(rec.fn);
  var src = noteOn(0, 5, 60, 0xffff);
  var snapshot = Array.from(src);
  p.send(src);
  p.send(pnBend(0, 5, 60, 0xc0000000));
  assert.deepEqual(Array.from(src), snapshot);
  assert.equal(src.gr, undefined);
  assert.deepEqual(src._from, []);
});

test('M2M1() without arguments keeps the original conversion and message order', function() {
  var rec = recorder();
  var p = JZZ.M2M1();
  p.connect(rec.fn);
  p.send(ump([0x42, 0x93, 60, 0, 0x80, 0, 0, 0]));
  p.send(ump([0x42, 0x83, 60, 0, 0x40, 0, 0, 0]));
  p.send(ump([0x42, 0x63, 60, 0, 0xc0, 0, 0, 0])); // per-note bend still ignored
  p.send(ump([0x42, 0xa3, 60, 0, 0xa0, 0, 0, 0]));
  p.send(ump([0x42, 0xe3, 0, 0, 0, 0, 0, 0]));
  p.send(ump([0x42, 0xd3, 0, 0, 0xa0, 0, 0, 0]));
  assert.deepEqual(rec.log, [
    { bytes: [0x93, 60, 64], gr: 2 },
    { bytes: [0x83, 60, 32], gr: 2 },
    { bytes: [0xa3, 60, 80], gr: 2 },
    { bytes: [0xe3, 0, 0], gr: 2 },
    { bytes: [0xd3, 80], gr: 2 }
  ]);
});

test('chained send/connect and MIDI2 helpers keep working in MPE mode', function() {
  var rec = recorder();
  var p = JZZ.M2M1({ mpe: { master: 0, members: [1, 2], bendRange: 48 } });
  p.connect(rec.fn)
    .MIDI2().umpNoteOn(0, 5, 60, 0xffff)
    .umpPnPitchBendF(0, 5, 60, 0.5)
    .umpNoteOff(0, 5, 60);
  var out = bytes(rec.log);
  assert.deepEqual(out[out.length - 1], [0x81, 60, 0]);
  assert.ok(out.some(function(m) { return m[0] == 0x91 && m[1] == 60; }));
  assert.ok(out.some(function(m) { return m[0] == 0xe1; }));
});
