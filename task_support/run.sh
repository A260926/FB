#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
mkdir -p .task-build
rm -rf .task-build/gomodcache .task-build/gocache
mkdir -p .task-build/gomodcache .task-build/gocache
export GOMODCACHE="$PWD/.task-build/gomodcache" GOCACHE="$PWD/.task-build/gocache" GOPROXY=off GOTOOLCHAIN=local GOFLAGS=-mod=vendor GOSUMDB=off
if [[ $mode == target ]]; then
  test -s client_fmp4_epoch_task_test.go
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  (
set -e
test -s client_fmp4_epoch.go
go list -f '{{range .GoFiles}}{{println .}}{{end}}' . | grep -qx client_fmp4_epoch.go
test -s task_support/epoch_probe/main.go
go run ./task_support/epoch_probe
go test -count=1 -timeout=90s -v -run '^(TestEpochTimeline|TestEpochRenditions|TestEpochCancel)$' .
) >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in TestEpochTimeline TestEpochRenditions TestEpochCancel; do
    grep -Eq "^=== RUN +${group}$" "$output_log"
    grep -Eq "^--- PASS: ${group} " "$output_log"
    if grep -Eq "^[[:space:]]*--- (SKIP|FAIL): ${group}([/ ]|$)" "$output_log"; then exit 4; fi
    grep -Eq "TASK_GROUP_DONE ${group} checks=[1-9][0-9]*$" "$output_log"
  done
  grep -qx 'PASS' "$output_log"
fi
if [[ $mode == baseline ]]; then
  go run ./task_support/baseline
else
  go run ./task_support/baseline -normal-only
fi
