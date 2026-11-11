#!/usr/bin/env sh
#
# Parameter copy-in / copy-out QA.
#
# Runs, against the unmodified source:
#   1. devcheck/baseline.mjs   (the three runnable examples from the task)
#   2. devcheck/parameter_copy.test.mjs (the new numeric regression suite)
#   3. node test/functions.js
#   4. node test/structs.js
#
# Then, for each of three isolated behavioural regressions, it builds an
# independent temporary copy of the compiler under devcheck/qa-artifacts,
# applies exactly one patch, and runs the same numeric suite in capture mode.
# Capture is accepted only when every fixture still compiles and executes,
# control shaders (no output parameters) still pass, and the expected case
# fails with a numeric mismatch in both optimize modes.  Compile errors,
# import failures, timeouts, zero executions and missing hits are failures,
# never successful captures.
#
# The original compiler sources are never modified: patched files live only in
# the temporary copies.  Patch files and per-case reports are retained under
# devcheck/qa-artifacts.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$ROOT"

ARTIFACTS=${PARAMETER_COPY_ARTIFACTS_DIR:-"$ROOT/devcheck/qa-artifacts"}
TIMEOUT_SECONDS=${PARAMETER_COPY_TIMEOUT:-120}
FIXTURE_DIR="$ROOT/devcheck/fixtures/parameter-copy"
PATCH_DIR="$ROOT/devcheck/parameter-copy-patches"

fail() {
	printf 'PARAMETER_COPY_QA_FAIL %s\n' "$*" >&2
	exit 1
}

# Portable per-command timeout: perl's alarm survives exec of the child.
run_bounded() {
	bound_label=$1
	bound_seconds=$2
	shift 2
	bound_status=0
	perl -e 'alarm shift; exec @ARGV' "$bound_seconds" "$@" >"$ARTIFACTS/$bound_label.log" 2>&1 || bound_status=$?
	if [ "$bound_status" -eq 142 ] || [ "$bound_status" -eq 143 ]; then
		fail "timeout label=$bound_label seconds=$bound_seconds"
	fi
	return "$bound_status"
}

json_field() {
	# json_field <file> <expression over parsed object `p`>
	node -e "try{const fs=require('fs');const p=JSON.parse(fs.readFileSync(process.argv[1],'utf8'));process.stdout.write(String($2))}catch(e){process.stdout.write('unreadable')}" "$1"
}

mkdir -p "$ARTIFACTS"
rm -rf "$ARTIFACTS/copies"
rm -f "$ARTIFACTS"/*.log "$ARTIFACTS"/*.json "$ARTIFACTS"/*.patch "$ARTIFACTS/RESULTS.txt" 2>/dev/null || true

# Record hashes of the tracked compiler sources so we can prove they were not
# mutated by any of the patch runs.
BEFORE_HASHES=$(git hash-object index.js lib/index.js lib/stdlib300.js)

printf '== unmodified source ==\n'

if ! run_bounded baseline "$TIMEOUT_SECONDS" node devcheck/baseline.mjs; then
	fail "baseline_examples_failed log=$ARTIFACTS/baseline.log"
fi

if ! run_bounded parameter-normal "$TIMEOUT_SECONDS" env \
	PARAMETER_COPY_RESULTS="$ARTIFACTS/normal-parameter-copy.json" \
	node devcheck/parameter_copy.test.mjs; then
	fail "normal_parameter_copy_test_failed log=$ARTIFACTS/parameter-normal.log"
fi
if [ ! -s "$ARTIFACTS/normal-parameter-copy.json" ]; then
	fail "normal_parameter_copy_no_results log=$ARTIFACTS/parameter-normal.log"
fi

if ! run_bounded functions "$TIMEOUT_SECONDS" node test/functions.js; then
	fail "test_functions_js_failed log=$ARTIFACTS/functions.log"
fi
if ! run_bounded structs "$TIMEOUT_SECONDS" node test/structs.js; then
	fail "test_structs_js_failed log=$ARTIFACTS/structs.log"
fi

CASE_COUNT=$(json_field "$ARTIFACTS/normal-parameter-copy.json" 'p.caseCount')
CONTROL_COUNT=$(json_field "$ARTIFACTS/normal-parameter-copy.json" 'p.controls')
EXECUTED_COUNT=$(json_field "$ARTIFACTS/normal-parameter-copy.json" 'p.summary.executed')
FAILED_COUNT=$(json_field "$ARTIFACTS/normal-parameter-copy.json" 'p.summary.failed')
if [ "$FAILED_COUNT" != "0" ]; then
	fail "normal_parameter_copy_reported_failures count=$FAILED_COUNT"
fi
if [ "$EXECUTED_COUNT" = "0" ]; then
	fail "normal_parameter_copy_zero_executions"
fi
if [ "$CASE_COUNT" -lt 12 ]; then
	fail "normal_parameter_copy_too_few_cases count=$CASE_COUNT"
fi

# One case file per named case, so the fixture count is a useful cross-check.
set -- "$FIXTURE_DIR"/*.glsl
FIXTURE_COUNT=$#
if [ "$CASE_COUNT" -gt "$FIXTURE_COUNT" ]; then
	fail "case_count_exceeds_fixtures cases=$CASE_COUNT fixtures=$FIXTURE_COUNT"
fi

printf 'normal: cases=%s controls=%s executed=%s\n' "$CASE_COUNT" "$CONTROL_COUNT" "$EXECUTED_COUNT"

# Patch name | fixtures that must fail numerically in both optimize modes.
# Each patch changes exactly one behaviour and touches exactly one source file.
for spec in \
	"shallow-inner-copy|in_struct_unchanged,in_struct_array_unchanged,struct_by_value_return" \
	"reevaluate-output-index|dynamic_index_single_eval" \
	"nonvoid-early-return-no-writeback|inout_early_return_value,out_early_return_value"
do
	PATCH_NAME=${spec%%|*}
	EXPECTED_HITS=${spec#*|}

	if [ ! -f "$PATCH_DIR/$PATCH_NAME.patch" ]; then
		fail "missing_patch:$PATCH_NAME"
	fi

	printf '== patched copy: %s ==\n' "$PATCH_NAME"

	WORK="$ARTIFACTS/copies/$PATCH_NAME"
	mkdir -p "$WORK/lib" "$WORK/devcheck/fixtures/parameter-copy"
	cp "$ROOT/index.js" "$ROOT/stream.js" "$ROOT/package.json" "$WORK/"
	cp "$ROOT"/lib/*.js "$WORK/lib/"
	cp "$ROOT/devcheck/parameter_copy.test.mjs" "$WORK/devcheck/"
	cp "$FIXTURE_DIR"/*.glsl "$WORK/devcheck/fixtures/parameter-copy/"

	if ! patch -p1 -d "$WORK" < "$PATCH_DIR/$PATCH_NAME.patch" >"$ARTIFACTS/$PATCH_NAME.apply.log" 2>&1; then
		fail "patch_apply_failed:$PATCH_NAME log=$ARTIFACTS/$PATCH_NAME.apply.log"
	fi
	# Retain the exact patch that was exercised.
	cp "$PATCH_DIR/$PATCH_NAME.patch" "$ARTIFACTS/$PATCH_NAME.patch"

	if ! run_bounded "capture-$PATCH_NAME" "$TIMEOUT_SECONDS" env \
		PARAMETER_COPY_CAPTURE=1 \
		PARAMETER_COPY_EXPECT_HITS="$EXPECTED_HITS" \
		PARAMETER_COPY_PATCH_LABEL="$PATCH_NAME" \
		PARAMETER_COPY_RESULTS="$ARTIFACTS/capture-$PATCH_NAME.json" \
		node "$WORK/devcheck/parameter_copy.test.mjs"; then
		CAPTURE_REASON=no_results_or_import_failure
		if [ -s "$ARTIFACTS/capture-$PATCH_NAME.json" ]; then
			CAPTURE_REASON=$(json_field "$ARTIFACTS/capture-$PATCH_NAME.json" "p.capture?.reason||'unknown'")
		fi
		fail "capture_invalid:$PATCH_NAME reason=$CAPTURE_REASON log=$ARTIFACTS/capture-$PATCH_NAME.log"
	fi
	if [ ! -s "$ARTIFACTS/capture-$PATCH_NAME.json" ]; then
		fail "capture_zero_results:$PATCH_NAME log=$ARTIFACTS/capture-$PATCH_NAME.log"
	fi

	CAPTURE_CONTROLS=$(json_field "$ARTIFACTS/capture-$PATCH_NAME.json" 'p.summary.controlsPassed')
	CAPTURE_EXECUTED=$(json_field "$ARTIFACTS/capture-$PATCH_NAME.json" 'p.summary.executed')
	CAPTURE_COMPILED=$(json_field "$ARTIFACTS/capture-$PATCH_NAME.json" 'p.summary.compiled')
	CAPTURE_RECORDS=$(json_field "$ARTIFACTS/capture-$PATCH_NAME.json" 'p.records.length')
	CAPTURE_FAILED=$(json_field "$ARTIFACTS/capture-$PATCH_NAME.json" 'p.summary.failed')
	if [ "$CAPTURE_EXECUTED" = "0" ]; then
		fail "capture_zero_executions:$PATCH_NAME"
	fi
	if [ "$CAPTURE_CONTROLS" = "0" ]; then
		fail "capture_controls_not_exercised:$PATCH_NAME"
	fi
	if [ "$CAPTURE_FAILED" = "0" ]; then
		fail "capture_no_numeric_mismatch:$PATCH_NAME"
	fi

	printf 'patch %s: capture=ok compiled=%s/%s executed=%s/%s controls-passed=%s hits=%s\n' \
		"$PATCH_NAME" "$CAPTURE_COMPILED" "$CAPTURE_RECORDS" "$CAPTURE_EXECUTED" "$CAPTURE_RECORDS" \
		"$CAPTURE_CONTROLS" "$EXPECTED_HITS"
done

AFTER_HASHES=$(git hash-object index.js lib/index.js lib/stdlib300.js)
if [ "$BEFORE_HASHES" != "$AFTER_HASHES" ]; then
	fail "original_source_was_modified"
fi

# The temporary patched trees served their purpose; retain the patches, logs
# and per-case JSON reports, but not stale copies of a deliberately broken
# compiler.
rm -rf "$ARTIFACTS/copies"

# Short human-readable summary; full per-case records live in the JSON files.
{
	printf 'parameter-copy QA summary\n'
	printf 'normal: cases=%s controls=%s executed=%s failed=0 optimize=false/true=equal\n' \
		"$CASE_COUNT" "$CONTROL_COUNT" "$EXECUTED_COUNT"
	printf 'baseline: pass\ntest/functions.js: pass\ntest/structs.js: pass\n'
	printf 'patch shallow-inner-copy: detected (deep copy of struct members)\n'
	printf 'patch reevaluate-output-index: detected (single evaluation of a[k++].y)\n'
	printf 'patch nonvoid-early-return-no-writeback: detected (early-return out/inout write-back)\n'
} > "$ARTIFACTS/RESULTS.txt"

printf 'PARAMETER_COPY_QA_OK cases=%s fixtures=%s patches=3 controls=%s optimized-equal=yes\n' \
	"$CASE_COUNT" "$FIXTURE_COUNT" "$CONTROL_COUNT"
