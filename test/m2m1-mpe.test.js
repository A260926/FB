// MPE output mode tests for JZZ.M2M1({ mpe: { master, members, bendRange } })
// Run with: node --test test/m2m1-mpe.test.js
const assert = require('node:assert');
const test = require('node:test');
const JZZ = require('..');

// byte-level recorder: every received MIDI message is stored as a plain byte array
function recorder(port) {
  const log = [];
  port.connect(function(msg) { log.push(Array.prototype.slice.call(msg)); });
  return log;
}
const wheels = function(log) {
  return log.filter(function(m) { return (m[0] & 0xf0) == 0xe0; }).map(function(m) { return m[1] + m[2] * 128; });
};
const noteOns = function(log) { return log.filter(function(m) { return (m[0] & 0xf0) == 0x90 && m[2]; }); };
const noteOffs = function(log) { return log.filter(function(m) { return (m[0] & 0xf0) == 0x80; }); };
const rpnInit = function(ch, range) {
  const b = 0xb0 + ch;
  return [[b, 101, 0], [b, 100, 0], [b, 6, range], [b, 38, 0], [b, 101, 127], [b, 100, 127]];
};
const preset = function(ch, cc1, cc11) {
  return [[0xb0 + ch, 1, cc1], [0xb0 + ch, 11, cc11], [0xd0 + ch, 0], [0xe0 + ch, 0, 64]];
};

test('MPE config is validated at construction and nothing is emitted', () => {
  const bad = [
    { master: 0, members: [1, 1], bendRange: 48 },          // duplicate member
    { master: 1, members: [1], bendRange: 48 },             // member includes master
    { master: 0, members: [], bendRange: 48 },              // empty pool
    { master: 16, members: [1], bendRange: 48 },            // master out of range
    { master: -1, members: [1], bendRange: 48 },
    { master: 0, members: [16], bendRange: 48 },            // member out of range
    { master: 0, members: [1], bendRange: 0 },              // range too small
    { master: 0, members: [1], bendRange: 97 },             // range too big
    { master: 1.5, members: [1], bendRange: 48 },           // non-integer master
    { master: 0, members: [1], bendRange: 48.5 },           // non-integer range
    { master: 0, members: ['x'], bendRange: 48 },           // non-integer member
    {}, null, [], 'x'                                        // malformed config
  ];
  bad.forEach(function(cfg) {
    assert.throws(function() { new JZZ.M2M1({ mpe: cfg }); }, RangeError);
  });
  assert.throws(function() { new JZZ.M2M1({ mpe: null }); }, RangeError);
  // valid configs construct fine
  new JZZ.M2M1({ mpe: { master: 15, members: [0], bendRange: 1 } });
  new JZZ.M2M1({ mpe: { master: 0, members: [1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15], bendRange: 96 } });
  // no options / unrelated options keep the legacy bridge
  new JZZ.M2M1();
  new JZZ.M2M1({});
  new JZZ.M2M1({ foo: 1 });
});

test('same pitch on two groups and two source channels bends independently', () => {
  const port = new JZZ.M2M1({ mpe: { master: 0, members: [1, 2], bendRange: 48 } });
  const log = recorder(port);
  // two groups, same source channel and pitch
  port.umpNoteOn(0, 3, 60);
  port.umpNoteOn(1, 3, 60);
  assert.deepStrictEqual(noteOns(log), [[0x91, 60, 127], [0x92, 60, 127]]);
  // same group, two source channels, same pitch: third voice steals the oldest one
  port.umpNoteOn(0, 4, 60);
  assert.deepStrictEqual(noteOffs(log), [[0x81, 60, 0]]);
  assert.deepStrictEqual(noteOns(log).slice(-1)[0], [0x91, 60, 127]);
  // bend of the group-1/channel-3 voice must only touch member 2
  const mark = log.length;
  port.umpPnPitchBend(1, 3, 60, 0xffffffff);
  const bent = log.slice(mark);
  assert.strictEqual(bent.length, 1);
  assert.strictEqual(bent[0][0], 0xe2);
  assert.strictEqual(wheels(bent)[0], 0x3fff);
  // bend of group-0/channel-4 voice (now on member 1) must only touch member 1
  const mark2 = log.length;
  port.umpPnPitchBend(0, 4, 60, 0);
  assert.strictEqual(log.slice(mark2)[0][0], 0xe1);
  assert.strictEqual(wheels(log.slice(mark2))[0], 0);
  // late note-off of the stolen group-0/channel-3 voice is ignored
  const mark3 = log.length;
  port.umpNoteOff(0, 3, 60);
  port.umpPnPressure(0, 3, 60, 0xffffffff);
  assert.strictEqual(log.length, mark3);
});

test('per-note and channel pitch wheels are added, rounded, clamped and hit both ends', () => {
  const port = new JZZ.M2M1({ mpe: { master: 0, members: [1], bendRange: 48 } });
  const log = recorder(port);
  port.umpNoteOn(0, 3, 60);
  assert.deepStrictEqual(wheels(log), [8192]); // centered
  port.umpPnPitchBend(0, 3, 60, 0xffffffff);    // +48 semitones
  assert.strictEqual(wheels(log).slice(-1)[0], 0x3fff);
  port.umpPnPitchBend(0, 3, 60, 0);             // -48 semitones
  assert.strictEqual(wheels(log).slice(-1)[0], 0);
  port.umpPnPitchBend(0, 3, 60, 0x80000000);    // back to center
  assert.strictEqual(wheels(log).slice(-1)[0], 8192);
  port.umpPitchBend(0, 3, 0xffffffff);          // channel +2 semitones: 8192 + round(2/48*8192)
  assert.strictEqual(wheels(log).slice(-1)[0], 8192 + Math.round(2 / 48 * 8192));
  port.umpPnPitchBend(0, 3, 60, 0xffffffff);    // +48 +2 = 50 -> saturated at +48
  assert.strictEqual(wheels(log).slice(-1)[0], 0x3fff);
  port.umpPitchBend(0, 3, 0);                   // channel -2: +48 - 2 = 46, no longer saturated
  assert.strictEqual(wheels(log).slice(-1)[0], 8192 + Math.round(46 / 48 * 8192));
  port.umpPnPitchBend(0, 3, 60, 0);             // -48 -2 = -50 -> saturated at -48
  assert.strictEqual(wheels(log).slice(-1)[0], 0);
  // per-note offset survives channel wheel changes: pn stays at -48, channel back to center
  port.umpPitchBend(0, 3, 0x80000000);
  assert.strictEqual(wheels(log).slice(-1)[0], 0);
  port.umpPitchBend(0, 3, 0xffffffff); // -48 + 2 = -46, still driven by the stored per-note bend
  assert.strictEqual(wheels(log).slice(-1)[0], 8192 + Math.round(-46 / 48 * 8192));

  // bendRange 2: the channel wheel alone (2 semitones) already covers the full member range
  const p2 = new JZZ.M2M1({ mpe: { master: 0, members: [1], bendRange: 2 } });
  const l2 = recorder(p2);
  p2.umpNoteOn(0, 3, 60);
  p2.umpPitchBend(0, 3, 0);
  assert.strictEqual(wheels(l2).slice(-1)[0], 0);
  p2.umpPitchBend(0, 3, 0xffffffff);
  assert.strictEqual(wheels(l2).slice(-1)[0], 0x3fff);
});

test('channel wheel update keeps every per-note offset and touches all voices of the source', () => {
  const port = new JZZ.M2M1({ mpe: { master: 0, members: [1, 2], bendRange: 48 } });
  const log = recorder(port);
  port.umpNoteOn(0, 3, 60);
  port.umpNoteOn(0, 3, 64);
  port.umpPnPitchBend(0, 3, 60, 0xffffffff); // voice on member 1 at +48
  const mark = log.length;
  port.umpPitchBend(0, 3, 0xffffffff);       // channel +2 updates both, member 1 saturates
  const out = log.slice(mark);
  assert.strictEqual(out.length, 2);
  assert.strictEqual(out[0][0], 0xe1);
  assert.strictEqual(out[1][0], 0xe2);
  assert.strictEqual(wheels(out)[0], 0x3fff);                 // 48 + 2 clamped
  assert.strictEqual(wheels(out)[1], 8192 + Math.round(2 / 48 * 8192)); // centered voice +2
  // another source channel is not touched
  const mark2 = log.length;
  port.umpPitchBend(0, 9, 0xffffffff);
  assert.strictEqual(log.length, mark2);
});

test('full pool steals the oldest voice and late release/control cannot reach the new voice', () => {
  const port = new JZZ.M2M1({ mpe: { master: 0, members: [1, 2], bendRange: 48 } });
  const log = recorder(port);
  port.umpNoteOn(0, 3, 60);
  port.umpNoteOn(0, 3, 64);
  port.umpNoteOn(0, 3, 67); // pool full: steals note 60 on member 1
  const offs = noteOffs(log);
  assert.deepStrictEqual(offs, [[0x81, 60, 0]]);
  const stealAt = log.indexOf(offs[0]);
  const newOn = log.slice(stealAt + 1).filter(function(m) { return (m[0] & 0xf0) == 0x90; })[0];
  assert.deepStrictEqual(newOn, [0x91, 67, 127]);
  // the stolen voice's member was re-preset before the new NoteOn
  const between = log.slice(stealAt + 1, log.indexOf(newOn));
  assert.deepStrictEqual(between, preset(1, 0, 127));
  // late NoteOff and per-note control of the stolen note do nothing
  const mark = log.length;
  port.umpNoteOff(0, 3, 60);
  port.umpPnPitchBend(0, 3, 60, 0);
  port.umpPnPressure(0, 3, 60, 0xffffffff);
  assert.strictEqual(log.length, mark);
  // the new owner of member 1 still works
  port.umpPnPitchBend(0, 3, 67, 0xffffffff);
  assert.strictEqual(log.slice(mark)[0][0], 0xe1);
  // normal release frees the member
  port.umpNoteOff(0, 3, 67);
  assert.deepStrictEqual(noteOffs(log).slice(-1)[0], [0x81, 67, 0]);
  const mark2 = log.length;
  port.umpNoteOn(0, 3, 70); // reused member 1: no RPN re-init, but full preset
  const reuse = log.slice(mark2);
  assert.deepStrictEqual(reuse, preset(1, 0, 127).concat([[0x91, 70, 127]]));
});

test('same triplet retrigger ends the old note before restarting', () => {
  const port = new JZZ.M2M1({ mpe: { master: 0, members: [1], bendRange: 48 } });
  const log = recorder(port);
  port.umpNoteOn(0, 3, 60, 0x8000);
  port.umpNoteOn(0, 3, 60, 0xffff);
  const ons = noteOns(log);
  const offs = noteOffs(log);
  assert.deepStrictEqual(ons, [[0x91, 60, 64], [0x91, 60, 127]]);
  assert.deepStrictEqual(offs, [[0x81, 60, 0]]); // clean velocity-0 NoteOff
  assert.ok(log.indexOf(offs[0]) < log.indexOf(ons[1]));
  assert.deepStrictEqual(log.slice(log.indexOf(offs[0]) + 1, log.indexOf(ons[1])), preset(1, 0, 127));
  // late release of the first incarnation is ignored
  const mark = log.length;
  port.umpNoteOff(0, 3, 60); // releases the current incarnation
  assert.strictEqual(noteOffs(log).length, 2);
  const mark2 = log.length;
  port.umpNoteOff(0, 3, 60);
  port.umpPnPitchBend(0, 3, 60, 0);
  assert.strictEqual(log.length, mark2);
});

test('zero-velocity NoteOn acts as NoteOff and releases the member', () => {
  const port = new JZZ.M2M1({ mpe: { master: 0, members: [1], bendRange: 48 } });
  const log = recorder(port);
  port.umpNoteOn(0, 3, 60);
  port.send(JZZ.UMP([0x40, 0x93, 60, 0, 0, 0, 0, 0])); // status 9, velocity 0
  assert.deepStrictEqual(noteOffs(log), [[0x81, 60, 0]]);
  // member is free again: no RPN re-init on reuse
  const mark = log.length;
  port.umpNoteOn(0, 3, 60);
  assert.deepStrictEqual(log.slice(mark), preset(1, 0, 127).concat([[0x91, 60, 127]]));
});

test('CC1/CC11 are cached while idle, inherited by new notes and sent to active members', () => {
  const port = new JZZ.M2M1({ mpe: { master: 0, members: [1, 2], bendRange: 48 } });
  const log = recorder(port);
  // idle CC updates only change the cache: nothing is emitted
  port.umpControl(0, 3, 1, 0x80000000);  // -> 64
  port.umpControl(0, 3, 11, 0x40000000); // -> 32
  assert.strictEqual(log.length, 0);
  // the first note inherits the cached values instead of the defaults
  port.umpNoteOn(0, 3, 60);
  const firstPreset = log.filter(function(m) { return m[0] == 0xb1 && (m[1] == 1 || m[1] == 11); });
  assert.deepStrictEqual(firstPreset, [[0xb1, 1, 64], [0xb1, 11, 32]]);
  // active members receive further changes immediately
  const mark = log.length;
  port.umpControl(0, 3, 1, 0xffffffff); // -> 127
  assert.deepStrictEqual(log.slice(mark), [[0xb1, 1, 127]]);
  // a new note on the other member inherits the cached values
  port.umpNoteOn(0, 3, 64);
  const secondPreset = log.filter(function(m) { return m[0] == 0xb2 && (m[1] == 1 || m[1] == 11); });
  assert.deepStrictEqual(secondPreset, [[0xb2, 1, 127], [0xb2, 11, 32]]);
  // CC on another source channel stays isolated and cached silently
  const mark2 = log.length;
  port.umpControl(0, 9, 1, 0xffffffff);
  assert.strictEqual(log.length, mark2);
  // ...and that other source's new note inherits the value cached while it was idle
  const mark3 = log.length;
  port.umpNoteOn(0, 9, 70); // pool full: steals, but its preset shows source-9's cache
  const stolenPreset = log.slice(mark3).filter(function(m) { return m[0] == 0xb1 && (m[1] == 1 || m[1] == 11); });
  assert.deepStrictEqual(stolenPreset, [[0xb1, 1, 127], [0xb1, 11, 127]]);
});

test('per-note poly pressure becomes member channel pressure and is ignored without a voice', () => {
  const port = new JZZ.M2M1({ mpe: { master: 0, members: [1, 2], bendRange: 48 } });
  const log = recorder(port);
  const mark = log.length;
  port.umpPnPressure(0, 3, 60, 0xa0000000); // no active voice yet
  assert.strictEqual(log.length, mark);
  port.umpNoteOn(0, 3, 60);
  port.umpPnPressure(0, 3, 60, 0xa0000000); // 32-bit 0xa0... -> 7-bit 80
  port.umpAftertouch(0, 3, 60, 0);          // alias helper, zero
  assert.deepStrictEqual(
    log.filter(function(m) { return (m[0] & 0xf0) == 0xd0; }),
    [[0xd1, 0], [0xd1, 80], [0xd1, 0]]
  );
});

test('member bend range RPN is sent once, before the first NoteOn, and closed with null RPN', () => {
  const port = new JZZ.M2M1({ mpe: { master: 0, members: [1, 2], bendRange: 48 } });
  const log = recorder(port);
  port.umpNoteOn(0, 3, 60);
  const onAt = log.indexOf([0x91, 60, 127]);
  assert.deepStrictEqual(log.slice(0, onAt), rpnInit(1, 48).concat(preset(1, 0, 127)));
  port.umpNoteOff(0, 3, 60);
  const mark = log.length;
  port.umpNoteOn(0, 3, 60); // same member again: no RPN this time
  assert.deepStrictEqual(log.slice(mark), preset(1, 0, 127).concat([[0x91, 60, 127]]));
  // second member gets its own one-time RPN
  port.umpNoteOn(0, 3, 64);
  const on2 = log.indexOf([0x92, 64, 127]);
  assert.deepStrictEqual(log.slice(on2 - 10, on2 - 4), rpnInit(2, 48));
});

test('two receivers get equal, non-aliased output and synchronous echoes keep batch order', () => {
  const port = new JZZ.M2M1({ mpe: { master: 0, members: [1, 2], bendRange: 48 } });
  const a = [];
  const b = [];
  port.connect(function(msg) {
    a.push(Array.prototype.slice.call(msg));
    if ((msg[0] & 0xf0) == 0x90 && msg[2]) port.umpNoteOff(0, 3, 60); // synchronous echo
  });
  port.connect(function(msg) { b.push(Array.prototype.slice.call(msg)); });
  port.umpNoteOn(0, 3, 60);
  assert.deepStrictEqual(a, b); // identical content
  assert.ok(a.every(function(m, i) { return m !== b[i]; })); // never aliased
  // receiver B must see the whole NoteOn batch (last message = NoteOn) before the echoed NoteOff
  const noteOnIdx = b.findIndex(function(m) { return (m[0] & 0xf0) == 0x90 && m[2]; });
  const noteOffIdx = b.findIndex(function(m) { return (m[0] & 0xf0) == 0x80; });
  assert.ok(noteOnIdx >= 0 && noteOffIdx > noteOnIdx);
  assert.strictEqual(noteOnIdx, b.length - 2); // NoteOn is penultimate, echoed NoteOff last
});

test('MPE instances keep independent state', () => {
  const p1 = new JZZ.M2M1({ mpe: { master: 0, members: [1], bendRange: 48 } });
  const p2 = new JZZ.M2M1({ mpe: { master: 0, members: [1], bendRange: 48 } });
  const l1 = recorder(p1);
  const l2 = recorder(p2);
  p1.umpNoteOn(0, 3, 60);
  p2.umpNoteOn(7, 9, 100); // same member number, separate bridge: its own RPN init
  assert.deepStrictEqual(l2.slice(0, 6), rpnInit(1, 48));
  assert.deepStrictEqual(noteOns(l2), [[0x91, 100, 127]]);
  // filling p2's pool does not touch p1
  p2.umpNoteOn(0, 3, 61);
  assert.deepStrictEqual(noteOffs(l2), [[0x81, 100, 0]]);
  assert.strictEqual(noteOffs(l1).length, 0);
});

test('M2M1 without options keeps the legacy conversion and message order', () => {
  const port = new JZZ.M2M1();
  const log = recorder(port);
  port.send(JZZ.UMP.noteOn(0, 1, 'C5'))
    .umpNoteOff(0, 1, 'C5')
    .umpPitchBend(0, 1, 0xffffffff)
    .umpControl(0, 1, 1, 0xa0000000)
    .umpProgram(0, 1, 32)
    .send([0x40, 0xb1, 0, 0, 20, 20, 20, 20]); // unsupported CC stays ignored as before
  assert.deepStrictEqual(log, [
    [0x91, 0x3c, 0x7f],
    [0x81, 0x3c, 0],
    [0xe1, 0x7f, 0x7f],
    [0xb1, 1, 80],
    [0xc1, 32]
  ]);
  // SysEx conversion still works in MPE-less mode
  const p2 = new JZZ.M2M1({});
  const l2 = recorder(p2);
  p2.sxGM(0);
  assert.deepStrictEqual(l2, [[0xf0, 0x7e, 0x7f, 0x09, 0x01, 0xf7]]);
  // plain MIDI 1.0 input passes through untouched
  const p3 = new JZZ.M2M1();
  const l3 = recorder(p3);
  p3.send(JZZ.MIDI(0x90, 0x3c, 0x7f));
  assert.deepStrictEqual(l3, [[0x90, 0x3c, 0x7f]]);
});

test('unsupported type-4 messages are ignored in MPE mode while system/SysEx still pass', () => {
  const port = new JZZ.M2M1({ mpe: { master: 0, members: [1], bendRange: 48 } });
  const log = recorder(port);
  port.umpRPN(0, 3, 0, 0, 0);          // registered controller
  port.umpNRPN(0, 3, 0, 0, 0);         // assignable controller
  port.umpPnRPN(0, 3, 60, 0, 0);       // per-note registered controller
  port.umpPnManagement(0, 3, 60, 'SD'); // per-note management
  port.umpProgram(0, 3, 5);            // program change
  port.umpPressure(0, 3, 0x80000000);  // channel pressure
  port.umpControl(0, 3, 64, 0x80000000); // damper pedal
  assert.strictEqual(log.length, 0);
  port.sxGM(0);
  assert.deepStrictEqual(log, [[0xf0, 0x7e, 0x7f, 0x09, 0x01, 0xf7]]);
  port.send([0x10, 0xf8, 0, 0]); // timing clock (type 1)
  assert.deepStrictEqual(log.slice(-1)[0], [0xf8, 0, 0]);
});

test('the input message object and its stamp are not modified', () => {
  const port = new JZZ.M2M1({ mpe: { master: 0, members: [1], bendRange: 48 } });
  recorder(port);
  const msg = JZZ.UMP.umpNoteOn(0, 3, 60, 0xffff);
  const bytes = msg.slice();
  const stamp = {};
  msg._stamp(stamp);
  port.send(msg);
  assert.deepStrictEqual(msg.slice(), bytes);
  assert.ok(msg._stamped(stamp));
  port.send(msg); // second send must not grow the stamp list
  assert.ok(msg._stamped(stamp));
});
