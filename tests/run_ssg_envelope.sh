#!/usr/bin/env bash
#
# SSG-EG regression orchestration.
#
# 1. Builds tests/ssg_envelope.cpp against the unmodified library and runs all
#    six test groups (eight-shape trajectories, constructed boundary, release,
#    dynamic registers, SSG-disabled control, real YM2612 audio path).
# 2. Copies the sources into three temporary trees, applies one of the
#    baseline/mutations patches in each, rebuilds the *same* test binary and
#    confirms each mutation is rejected by a numeric envelope/phase assertion.
#
# A mutation only counts as detected when the mutated build compiles and the
# test prints a real "FAIL ..." assertion line; a compile error, a missing/
# skipped test group, or matching the copy's name are never treated as
# detection.
#
# No production source is modified in place and no network/device is needed.

set -euo pipefail

script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
root=$(cd "$script_dir/.." && pwd)
cxx=${CXX:-c++}

work=$(mktemp -d "${TMPDIR:-/tmp}/ssg_envelope.XXXXXX")
trap 'rm -rf "$work"' EXIT

test_src="$root/tests/ssg_envelope.cpp"
extra_src="$root/src/ymfm_adpcm.cpp $root/src/ymfm_ssg.cpp"
# the six PASS markers the test binary must emit on a healthy build
pass_groups=(
	"PASS shape_trajectories"
	"PASS constructed_boundary"
	"PASS release_behavior"
	"PASS dynamic_register_changes"
	"PASS ssg_disabled_control"
	"PASS audio_generate_path"
)

fail() { echo "ERROR: $*" >&2; exit 1; }

#-------------------------------------------------
# Phase 1: unmodified library
#-------------------------------------------------

echo "==> building SSG-EG tests against the original library"
"$cxx" -std=c++14 -O1 -Wall -I"$root/src" "$test_src" $extra_src -o "$work/ssg_test"

echo "==> running SSG-EG tests on the original library"
orig_log="$work/original.log"
"$work/ssg_test" | tee "$orig_log"

grep -q "SSG_ENVELOPE_ALL_PASS" "$orig_log" || fail "original run did not report SSG_ENVELOPE_ALL_PASS"

missing=0
for marker in "${pass_groups[@]}"; do
	if ! grep -q "$marker" "$orig_log"; then
		echo "ERROR: missing group marker: $marker" >&2
		missing=1
	fi
done
[ "$missing" -eq 0 ] || fail "one or more test groups did not run"

# the eight shapes are exercised inside shape_trajectories; require evidence
# that all modes 0-7 were evaluated rather than a silently shortened loop
summary=$(grep -E "^SUMMARY groups=" "$orig_log") || fail "no SUMMARY line"
echo "==> $summary"
echo "$summary" | grep -q "groups=6 " || fail "expected 6 executed groups"
echo "$summary" | grep -q "failures=0" || fail "original library reported failures"
checks=$(echo "$summary" | sed -E 's/.*checks=([0-9]+).*/\1/')
[ "$checks" -ge 200 ] || fail "only $checks assertions ran; groups appear to be skipped"

#-------------------------------------------------
# Phase 2: mutation copies
#-------------------------------------------------

# mutation name -> patch file; detection is accepted only from an actual FAIL
# assertion emitted by the rebuilt test
mutations=(alternate release phase)

for name in "${mutations[@]}"; do
	patch_file="$root/baseline/mutations/$name.patch"
	copy="$work/copy_$name"
	echo "==> mutation '$name': copying sources and applying $patch_file"
	rm -rf "$copy"
	cp -R "$root/src" "$copy"
	[ -f "$patch_file" ] || fail "missing patch $patch_file"
	# patches name a/src/ymfm_fm.ipp; the copy holds the src contents directly,
	# so strip two path components and apply inside the copy
	patch -p2 -d "$copy" < "$patch_file" >/dev/null

	bin="$work/mut_$name"
	log="$work/mut_$name.log"
	if ! "$cxx" -std=c++14 -O1 -Wall -I"$copy" "$test_src" \
			"$copy/ymfm_adpcm.cpp" "$copy/ymfm_ssg.cpp" -o "$bin" 2> "$work/mut_$name.build"; then
		cat "$work/mut_$name.build" >&2
		fail "mutation '$name' failed to compile; compile errors do not count as detection"
	fi

	# the test returns 1 on assertion failures, 2 on missing/skipped groups
	set +e
	"$bin" > "$log" 2>&1
	rc=$?
	set -e
	[ "$rc" -eq 1 ] || fail "mutation '$name' exit=$rc (expected assertion-failure exit 1)"

	first_fail=$(grep -m1 '^FAIL group=' "$log") || true
	[ -n "$first_fail" ] || fail "mutation '$name' produced no numeric FAIL assertion"

	# every FAIL line must name a concrete field with expected/actual values;
	# reject logs that only complain about skipped groups
	if grep -q '^FAIL .*field=' "$log"; then
		:
	else
		fail "mutation '$name' did not fail on an envelope/phase value assertion"
	fi

	echo "DETECTED_$name"
	echo "  first failure: $first_fail"
done

echo "SSG_ENVELOPE_MUTATIONS_DETECTED alternate release phase"
echo "SSG_ENVELOPE_RUN_OK"
