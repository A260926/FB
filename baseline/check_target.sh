#!/bin/bash
# baseline/check_target.sh — verify the floor 0 fix.
#
#   1. The three new regression tests in floor0_regression_test.go must
#      really run and pass (not skip).
#   2. The existing TestDecode and TestIMDCT must still pass.
#   3. The baseline record (baseline/run.sh) must show the corrected
#      observations: map1=16 and the cumulative LSP sequence 0.2, 0.4, 0.6.
set -euo pipefail
cd "$(dirname "$0")/.."

echo "== floor 0 regression tests =="
status=0
out=$(go test -count=1 -v -run '^(TestFloor0Coefficients|TestFloor0Curve|TestFloor0Packets)$' . 2>&1) || status=$?
echo "$out"
if [ "$status" -ne 0 ]; then
	echo "check_target: floor 0 regression tests failed" >&2
	exit 1
fi
for name in TestFloor0Coefficients TestFloor0Curve TestFloor0Packets; do
	if echo "$out" | grep -q -- "--- SKIP: $name"; then
		echo "check_target: $name was skipped" >&2
		exit 1
	fi
	if ! echo "$out" | grep -q -- "--- PASS: $name"; then
		echo "check_target: $name did not run and pass" >&2
		exit 1
	fi
done

echo "== existing tests (TestDecode, TestIMDCT) =="
go test -count=1 -run '^(TestDecode|TestIMDCT)$' .

echo "== baseline record =="
REQUIRE_CORRECT_FLOOR0=1 bash baseline/run.sh
if ! grep -q 'map1=16' baseline/record.txt; then
	echo "check_target: baseline record does not show map1=16" >&2
	exit 1
fi
if ! grep -q 'lsp=\[0.2 0.4 0.6\]' baseline/record.txt; then
	echo "check_target: baseline record does not show the cumulative LSP sequence" >&2
	exit 1
fi

echo "check_target: OK"
