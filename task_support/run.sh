#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
export GOPROXY=off GOSUMDB=off GOTOOLCHAIN=local GOFLAGS=-mod=vendor
task_cache=$(mktemp -d)
trap 'rm -rf "$task_cache"' EXIT
export GOMODCACHE="$task_cache/mod" GOCACHE="$task_cache/build"
mkdir -p "$GOMODCACHE" "$GOCACHE" .task-build

if [[ $mode == target ]]; then
  test -s avc_edits_test.go
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  (
set -e
test -s avc_edits.go
test -s internal/avcstate/state.go
test -s cmd/avcedits/main.go
go list -f '{{join .GoFiles " "}}' . | tr ' ' '\n' | grep -qx avc_edits.go
go list -f '{{join .GoFiles " "}}' ./internal/avcstate | tr ' ' '\n' | grep -qx state.go
go list -deps . | grep -qx github.com/abema/go-mp4/internal/avcstate
go build -o .task-build/api-normal ./task_support/api_consumer
.task-build/api-normal
go build -o .task-build/avcedits ./cmd/avcedits
rm -f .task-build/current.h264 .task-build/current.json
fixture_track=$(go run ./task_support/fixture_track)
.task-build/avcedits -track "$fixture_track" testdata/sample.mp4 .task-build/current.h264 .task-build/current.json
test -s .task-build/current.h264
test -s .task-build/current.json
.task-build/api-normal .task-build/current.h264 .task-build/current.json
go test -count=1 -v . -run '^(TestAVCStateReplay|TestAVCEditTiming|TestAVCEditReject)$'
) >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in TestAVCStateReplay TestAVCEditTiming TestAVCEditReject; do
    grep -Eq "^=== RUN +${group}$" "$output_log"
    grep -Eq "^--- PASS: ${group} " "$output_log"
    if grep -Eq "^[[:space:]]*--- (SKIP|FAIL): ${group}([/ ]|$)" "$output_log"; then exit 4; fi
    grep -Eq "TASK_GROUP_DONE ${group} checks=[1-9][0-9]*$" "$output_log"
  done
  grep -qx 'PASS' "$output_log"
fi
if [[ $mode == baseline ]]; then go run ./task_support/baseline; fi
