// In-memory regression baseline: no native MIDI plugins required.
// Verifies the core MIDI 2.0 <-> 1.0 bridges and the widget chaining
// keep working after the M2M1 MPE extension.
const assert = require('node:assert');
const JZZ = require('..');

let failures = 0;
function check(name, fn) {
  try {
    fn();
    console.log('ok - ' + name);
  }
  catch (err) {
    failures++;
    console.error('not ok - ' + name);
    console.error(err && err.stack ? err.stack : err);
  }
}

function bytes(port, fn) {
  const log = [];
  port.connect(function(msg) { log.push(Array.prototype.slice.call(msg)); });
  fn(port);
  return log;
}

check('M2M1 converts note on/off with 16-to-7 bit velocity', () => {
  const log = bytes(new JZZ.M2M1(), function(p) {
    p.umpNoteOn(0, 1, 'C5').umpNoteOff(0, 1, 'C5');
    p.send(JZZ.UMP([0x40, 0x91, 0x3c, 0, 0, 1, 0, 0])); // velocity 1 -> 1
  });
  assert.deepStrictEqual(log, [
    [0x91, 0x3c, 0x7f],
    [0x81, 0x3c, 0],
    [0x91, 0x3c, 1]
  ]);
});

check('M2M1 converts pitch bend, control change and program change', () => {
  const log = bytes(new JZZ.M2M1(), function(p) {
    p.umpPitchBend(0, 1, 0xffffffff)
      .umpControl(0, 1, 1, 0xa0000000)
      .umpProgram(0, 1, 32);
  });
  assert.deepStrictEqual(log, [
    [0xe1, 0x7f, 0x7f],
    [0xb1, 1, 80],
    [0xc1, 32]
  ]);
});

check('M2M1 keeps RPN ordering and sysex conversion', () => {
  const log = bytes(new JZZ.M2M1(), function(p) {
    p.umpRPN(0, 1, 2, 3, 0xf0f0f0f0);
    p.sxGM(0);
  });
  assert.deepStrictEqual(log[0], [0xb1, 0x65, 0x02]);
  assert.deepStrictEqual(log[1], [0xb1, 0x64, 0x03]);
  assert.deepStrictEqual(log[4], [0xf0, 0x7e, 0x7f, 0x09, 0x01, 0xf7]);
});

check('M2M1 passes plain MIDI 1.0 messages through', () => {
  const log = bytes(new JZZ.M2M1(), function(p) {
    p.send(JZZ.MIDI(0x90, 0x3c, 0x7f));
  });
  assert.deepStrictEqual(log, [[0x90, 0x3c, 0x7f]]);
});

check('M1M2 wraps MIDI 1.0 channel messages into UMP packages', () => {
  const log = bytes(new JZZ.M1M2(), function(p) {
    p.send(JZZ.MIDI(0x91, 0x3c, 0x7f));
  });
  assert.deepStrictEqual(log, [[0x20, 0x91, 0x3c, 0x7f]]);
});

check('widgets can be chained in memory and fan out one message', () => {
  const first = JZZ.Widget();
  const second = JZZ.Widget();
  const log = [];
  first.connect(second);
  second.connect(function(msg) { log.push(Array.prototype.slice.call(msg)); });
  first.send(JZZ.MIDI(0x92, 60, 100));
  assert.deepStrictEqual(log, [[0x92, 60, 100]]);
});

check('M2M1 MPE mode allocates members and still supports chaining', () => {
  const log = bytes(new JZZ.M2M1({ mpe: { master: 0, members: [1, 2], bendRange: 48 } }), function(p) {
    p.umpNoteOn(0, 3, 60).umpNoteOn(1, 3, 60).umpNoteOff(0, 3, 60);
  });
  const ons = log.filter(function(m) { return (m[0] & 0xf0) == 0x90; });
  const offs = log.filter(function(m) { return (m[0] & 0xf0) == 0x80; });
  assert.deepStrictEqual(ons, [[0x91, 60, 127], [0x92, 60, 127]]);
  assert.deepStrictEqual(offs, [[0x81, 60, 0]]);
});

if (failures) {
  console.error('BASELINE_FAIL: ' + failures + ' check(s) failed');
  process.exit(1);
}
console.log('BASELINE_PASS');
