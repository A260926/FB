#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
task_cache=$(mktemp -d "${TMPDIR:-/tmp}/dvb-task.XXXXXX")
export GOMODCACHE="$task_cache/modules" GOCACHE="$task_cache/build"
export GOPROXY=off GOTOOLCHAIN=local GOSUMDB=off GOFLAGS=-mod=vendor
mkdir -p "$GOMODCACHE" "$GOCACHE"
go build ./...
if [[ $mode == target ]]; then
  test -s dvbsub_test.go
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  (
set -e
test -s task_support/require_dvb_api.go.txt
mkdir -p .task-build
cp task_support/require_dvb_api.go.txt .task-build/require_dvb_api.go
go run .task-build/require_dvb_api.go
for source in dvbsub.go dvbsub_pixels.go examples/dvbsub/main.go; do test -s "$source"; done
files=$(go list -f '{{range .GoFiles}}{{println .}}{{end}}' .)
for source in dvbsub.go dvbsub_pixels.go; do printf '%s\n' "$files" | grep -qx "$source"; done
example_output=$(mktemp -d "${TMPDIR:-/tmp}/dvb-output.XXXXXX")
trap 'rm -rf "$example_output"' EXIT
go run ./examples/dvbsub task_support/fixtures/basic.ts "$example_output"
go run ./task_support/pamcheck "$example_output"
go test -count=1 -v -run '^(TestDVBDisplaySets|TestDVBIndexedPixels|TestDVBTransport|TestWebVTT)$' .
) >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in TestDVBDisplaySets TestDVBIndexedPixels TestDVBTransport; do
    grep -Eq "^=== RUN +${group}$" "$output_log"
    grep -Eq "^--- PASS: ${group} " "$output_log"
    if grep -Eq "^[[:space:]]*--- (SKIP|FAIL): ${group}([/ ]|$)" "$output_log"; then exit 4; fi
    grep -Eq "TASK_GROUP_DONE ${group} checks=[1-9][0-9]*$" "$output_log"
  done
  grep -qx 'PASS' "$output_log"
fi
go run ./task_support/baseline
rm -rf "$task_cache"
