#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
case "$mode" in baseline|target) ;; *) exit 2 ;; esac
task_cache=$(mktemp -d "${TMPDIR:-/tmp}/check-go.XXXXXX")
trap 'rm -rf "$task_cache"' EXIT
export GOCACHE="$task_cache/build" GOMODCACHE="$task_cache/modules" GOPROXY=off GOSUMDB=off GOTOOLCHAIN=local GOWORK=off
mkdir -p "$GOCACHE" "$GOMODCACHE"
go run task_support/overlay.go dwarf "$(go env GOROOT)/src/debug/dwarf" > task_support/overlay.json
go test -overlay=task_support/overlay.json -c -o task_support/dwarf.test debug/dwarf
if [[ $mode == target ]]; then
  test -s dwarf/supplementary_regression_test.go
  output_log=$(mktemp)
  trap 'rm -f "$output_log"; rm -rf "$task_cache"' EXIT
  set +e
  (cd dwarf && ../task_support/dwarf.test -test.v) >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in TestSupplementaryStrings TestSupplementaryTypes TestSupplementaryIsolation; do
    grep -Eq "^=== RUN +${group}$" "$output_log"
    grep -Eq "^--- PASS: ${group} " "$output_log"
    if grep -Eq "^[[:space:]]*--- (SKIP|FAIL): ${group}([/ ]|$)" "$output_log"; then exit 4; fi
    grep -Eq "TASK_GROUP_DONE ${group} checks=[1-9][0-9]*$" "$output_log"
  done
  grep -qx 'PASS' "$output_log"
fi
go run -overlay=task_support/overlay.json task_support/baseline/main.go
