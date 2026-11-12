#!/usr/bin/env bash
# Byte-level verification of the JZZ.M2M1() MPE output mode.
# Uses the Node.js built-in test runner and no real MIDI devices.
set -u

cd "$(dirname "$0")/.." || exit 1

TAP="${TMPDIR:-/tmp}/check_m2m1_mpe.$$.tap"
trap 'rm -f "$TAP"' EXIT

# A non-zero exit code on failure is expected; the TAP summary is parsed below.
node --test --test-reporter=tap test/m2m1-mpe.test.js >"$TAP" 2>&1 || true

node -e '
const fs = require("fs");
const tap = fs.readFileSync(process.argv[1], "utf8");
const pick = function(re) {
  var m = tap.match(re);
  return m ? parseInt(m[1], 10) : 0;
};
const tests = pick(/^# tests (\d+)/m);
const fail = pick(/^# fail (\d+)/m);
const skipped = pick(/^# skipped (\d+)/m);
if (tests < 8) {
  console.error("M2M1_MPE_FAIL: expected at least 8 executed tests, got " + tests);
  process.exit(1);
}
if (skipped !== 0) {
  console.error("M2M1_MPE_FAIL: " + skipped + " skipped test(s)");
  process.exit(1);
}
if (fail !== 0) {
  console.error("M2M1_MPE_FAIL: " + fail + " failed test(s)");
  process.exit(1);
}
' "$TAP" || exit 1

echo M2M1_MPE_PASS
