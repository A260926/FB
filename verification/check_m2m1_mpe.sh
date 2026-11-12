#!/usr/bin/env bash
# Runs the M2M1 MPE test suite with the Node.js built-in test runner
# and checks: at least 8 executed tests, zero skipped, zero failed.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
NODE_BIN="${NODE:-node}"

TAP="$($NODE_BIN --test --test-reporter=tap "$ROOT/test/m2m1-mpe.test.js")"
echo "$TAP"

TESTS="$(echo "$TAP" | sed -n 's/^# tests \([0-9][0-9]*\)$/\1/p' | tail -n 1)"
FAIL="$(echo "$TAP" | sed -n 's/^# fail \([0-9][0-9]*\)$/\1/p' | tail -n 1)"
SKIP="$(echo "$TAP" | sed -n 's/^# skipped \([0-9][0-9]*\)$/\1/p' | tail -n 1)"

if [ -z "$TESTS" ] || [ -z "$FAIL" ] || [ -z "$SKIP" ]; then
  echo "M2M1_MPE_FAIL: could not parse the test runner summary" >&2
  exit 1
fi
if [ "$TESTS" -lt 8 ]; then
  echo "M2M1_MPE_FAIL: expected at least 8 executed tests, got $TESTS" >&2
  exit 1
fi
if [ "$SKIP" -ne 0 ]; then
  echo "M2M1_MPE_FAIL: $SKIP skipped tests" >&2
  exit 1
fi
if [ "$FAIL" -ne 0 ]; then
  echo "M2M1_MPE_FAIL: $FAIL failing tests" >&2
  exit 1
fi

echo "M2M1_MPE_PASS"
