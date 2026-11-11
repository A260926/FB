#!/bin/sh
# Absorption test driver.
#
# Runs the new TestAbsorptionTarget group and the existing
# TestAbsorptionBaseline group through the real go test tool, verifies that
# each group exists, contains the expected cases and actually PASSes (a
# missing group or a group with zero cases is a failure), then builds the
# existing example via checks/baseline.sh. Per-case measured RGB and the
# analytical reference are printed by the tests themselves.
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$root"
export GO111MODULE=off
export PT_ABSORPTION_PPM="$root/test/absorption.ppm"

work=$(mktemp -d 2>/dev/null || mktemp -d "${TMPDIR:-/tmp}/ptabs.XXXXXX")
trap 'rm -rf "$work"' EXIT HUP INT TERM
rm -f "$PT_ABSORPTION_PPM"

# run_group <name> <minimum subtest count>
run_group() {
	group=$1
	min_subtests=$2
	out="$work/$group.out"
	# never let a non-zero go test exit abort the script before we validate
	if go test ./pt -run "^${group}\$" -count=1 -v >"$out" 2>&1; then
		status=0
	else
		status=$?
	fi
	cat "$out"

	runs=$(grep -c "=== RUN   ${group}" "$out" || true)
	if [ "$runs" -lt 1 ]; then
		echo "FAIL: group $group is missing (no RUN lines)" >&2
		return 1
	fi
	pass=$(grep -c "^--- PASS: ${group}" "$out" || true)
	if [ "$status" -ne 0 ] || [ "$pass" -lt 1 ]; then
		echo "FAIL: group $group did not PASS" >&2
		return 1
	fi
	subs=$(grep -c "^=== RUN   ${group}/" "$out" || true)
	if [ "$subs" -lt "$min_subtests" ]; then
		echo "FAIL: group $group has $subs subtests, want at least $min_subtests" >&2
		return 1
	fi
	echo "GROUP_OK $group subtests=$subs"
}

fail=0
run_group TestAbsorptionTarget 11 || fail=1
run_group TestAbsorptionBaseline 0 || fail=1

echo "--- checks/baseline.sh ---"
if sh checks/baseline.sh >"$work/baseline.out" 2>&1; then
	cat "$work/baseline.out"
else
	cat "$work/baseline.out"
	echo "FAIL: checks/baseline.sh failed" >&2
	exit 1
fi
grep -q PT_EXAMPLE_BUILD_OK "$work/baseline.out" || {
	echo "FAIL: example build marker PT_EXAMPLE_BUILD_OK missing" >&2
	exit 1
}

# the PPM must be produced by TestAbsorptionTarget from real Sample() pixels
if [ ! -s "$PT_ABSORPTION_PPM" ]; then
	echo "FAIL: absorption PPM was not produced at $PT_ABSORPTION_PPM" >&2
	exit 1
fi
echo "PPM_OK $PT_ABSORPTION_PPM ($(wc -c <"$PT_ABSORPTION_PPM" | tr -d ' ') bytes)"

if [ "$fail" -ne 0 ]; then
	echo "ABSORPTION_TESTS_FAILED" >&2
	exit 1
fi
echo "ABSORPTION_TESTS_OK"
