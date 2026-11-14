#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
build=${TASK_BUILD_DIR:-build/task}
case "$mode" in
 install) exec bash task_support/install.sh ;;
 baseline) filter='SteadyBaseline.*:Distribution1D.*:Triangle.Reintersect:Triangle.BadCases' ;;
 target)
  test -s src/tests/transient.cpp || { echo 'missing target src/tests/transient.cpp' >&2; exit 2; }
  filter='TransientPulse.*:TransientTransport.*:TransientFilm.*' ;;
 *) echo 'mode must be install, baseline, or target' >&2; exit 2 ;;
esac
# Recompile the declared target even after rapid edits with equal timestamps.
if [[ $mode == target ]]; then
 rm -f "$build/CMakeFiles/pbrt_test.dir/src/tests/transient.cpp.o" "$build/CMakeFiles/pbrt_test.dir/src/tests/transient.cpp.obj"
fi
mkdir -p "$(dirname "$build")"
bash task_support/install.sh > "$build-install.log" 2>&1 || { tail -80 "$build-install.log"; exit 1; }
mkdir -p "$build/task-results"
report=$(mktemp -d "$build/task-results/run.XXXXXX")
set +e
"$build/pbrt_test" --gtest_filter="$filter" --gtest_output="xml:$report/results.xml" > "$report/stdout.log" 2>&1
status=$?
set -e
cat "$report/stdout.log"
[[ $status -eq 0 ]] || exit "$status"
test -s "$report/results.xml" || { echo 'missing completed gtest XML' >&2; exit 3; }
grep -Eq '^\[  PASSED  \] [1-9][0-9]* tests?\.' "$report/stdout.log" || { echo 'no completed nonzero tests' >&2; exit 3; }
if grep -Eq '<failure|<error|<skipped|status="notrun"' "$report/results.xml"; then
 echo 'failed, skipped, or disabled test' >&2; exit 4
fi
if [[ $mode == target ]]; then
 for group in TransientPulse TransientTransport TransientFilm; do
  grep -Eq "^TASK_CASE_DONE $group[.][^ ]+ checks=[1-9][0-9]*$" "$report/stdout.log" || { echo "incomplete group $group" >&2; exit 4; }
  grep -Eq "<testcase .*status=\"run\".*classname=\"$group\"" "$report/results.xml" || { echo "zero executed cases $group" >&2; exit 4; }
 done
 bash task_support/run.sh baseline
 echo 'PBRT_TRANSIENT_TARGET_COMPLETE'
else
 grep -q '^PBRT_STEADY_MEDIUM_OK ' "$report/stdout.log"
 grep -q '^PBRT_STEADY_VOLPATH_OK rgb=3,3,3$' "$report/stdout.log"
 echo 'PBRT_STEADY_BASELINE_COMPLETE'
fi
