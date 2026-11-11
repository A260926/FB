#!/usr/bin/env bash
#
# Entry point for the order-independent JSCAD boolean spatial regression.
# Run from the repository root:
#
#   bash regress/check_booleans.sh
#
# It actually executes `node --test regress/boolean-regression.test.cjs` and
# only succeeds when all three named suites ran and passed:
#   - boolean-occupancy
#   - boolean-cavity-winding
#   - boolean-lazy-affine
#
# Zero tests, an all-skipped run, a failing assertion, or output that merely
# contains success text without a TAP plan / passing named tests all fail.
# On failure the full TAP output is printed, including the failing boolean
# expression, the transform, and the probe coordinate or volume difference.

set -u

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$REPO_ROOT" || { echo "cannot enter repo root $REPO_ROOT" >&2; exit 2; }

TEST_FILE="regress/boolean-regression.test.cjs"
REQUIRED_TESTS=(boolean-occupancy boolean-cavity-winding boolean-lazy-affine)

OUTPUT="$(mktemp "${TMPDIR:-/tmp}/boolean-regression.XXXXXX")"
trap 'rm -f "$OUTPUT"' EXIT

node --test --test-reporter=tap "$TEST_FILE" >"$OUTPUT" 2>&1
NODE_STATUS=$?

fail() {
  echo "boolean spatial regression FAIL: $1" >&2
  cat "$OUTPUT" >&2
  exit 1
}

[ "$NODE_STATUS" -eq 0 ] || fail "node --test exited with status $NODE_STATUS"

# A real test run must carry a TAP plan; a file with zero tests emits none.
grep -q '^1\.\.[0-9]' "$OUTPUT" || fail "no TAP test plan emitted (zero tests collected?)"

PASS_COUNT="$(sed -n 's/^# pass \([0-9][0-9]*\)$/\1/p' "$OUTPUT" | tail -1)"
FAIL_COUNT="$(sed -n 's/^# fail \([0-9][0-9]*\)$/\1/p' "$OUTPUT" | tail -1)"
SKIP_COUNT="$(sed -n 's/^# skipped \([0-9][0-9]*\)$/\1/p' "$OUTPUT" | tail -1)"
TODO_COUNT="$(sed -n 's/^# todo \([0-9][0-9]*\)$/\1/p' "$OUTPUT" | tail -1)"

[ -n "$PASS_COUNT" ] || fail "no pass summary in test output"
[ "${FAIL_COUNT:-0}" = "0" ] || fail "$FAIL_COUNT failing test(s)"
[ "${SKIP_COUNT:-0}" = "0" ] || fail "all or some tests were skipped"
[ "${TODO_COUNT:-0}" = "0" ] || fail "todo tests are not allowed in this gate"
[ "$PASS_COUNT" -ge "${#REQUIRED_TESTS[@]}" ] || fail "only $PASS_COUNT test(s) passed"

grep -q '^not ok ' "$OUTPUT" && fail "found a not-ok test result"

for name in "${REQUIRED_TESTS[@]}"; do
  grep -q "^ok [0-9][0-9]* - $name\$" "$OUTPUT" \
    || fail "required named test '$name' did not run and pass"
done

echo "boolean spatial regression PASS"
