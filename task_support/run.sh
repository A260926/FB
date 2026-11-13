#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
build=${TASK_BUILD_DIR:-build/task}
case "$mode" in
  install) exec bash task_support/install.sh ;;
  baseline) filter='EmitterBaseline.*:Distribution1D.*:Triangle.Reintersect:Triangle.BadCases' ;;
  target)
    test -s src/tests/emissive_map.cpp || { echo 'missing target src/tests/emissive_map.cpp' >&2; exit 2; }
    filter='EmissiveMapGeometry.*:EmissiveMapDensity.*:EmissiveMapTransport.*'
    ;;
  *) echo 'mode must be install, baseline, or target' >&2; exit 2 ;;
esac
mkdir -p "$(dirname "$build")"
bash task_support/install.sh > "$build-install.log" 2>&1 || { cat "$build-install.log"; exit 1; }
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
if [[ $mode == target ]]; then
  for group in EmissiveMapGeometry EmissiveMapDensity EmissiveMapTransport; do
    grep -Eq "^TASK_CASE_DONE $group[.][^ ]+ checks=[1-9][0-9]*$" "$report/stdout.log" || { echo "incomplete group $group" >&2; exit 4; }
    # XML is emitted by the fixed gtest runner only after all selected tests finish.
    grep -Eq "<testcase .*status=\"run\".*classname=\"$group\"" "$report/results.xml" || { echo "zero executed cases $group" >&2; exit 4; }
  done
  if grep -Eq '<failure|<error|<skipped|status="notrun"' "$report/results.xml"; then
    echo 'failed, skipped, or disabled target' >&2; exit 4
  fi
  bash task_support/run.sh baseline
  echo 'PBRT_EMISSIVE_TARGET_COMPLETE'
else
  grep -q '^PBRT_EMITTER_BASELINE_OK ' "$report/stdout.log"
  echo 'PBRT_SELECTED_LEGACY_COMPLETE'
fi
