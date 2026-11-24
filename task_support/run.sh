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
  test -s mp4/defragment_gaps_test.go
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  (
set -e
test -s mp4/defragment_gaps.go
test -s examples/defragmentgaps/main.go
go list -f '{{join .GoFiles " "}}' ./mp4 | tr ' ' '\n' | grep -qx defragment_gaps.go
go build -o .task-build/api-normal ./task_support/api_consumer
.task-build/api-normal
go build -o .task-build/gap-example ./examples/defragmentgaps
go build -o .task-build/defrag-cli ./cmd/mp4ff-defragment
rm -f .task-build/input-now.mp4 .task-build/example-now.mp4 .task-build/cli-now.mp4
go run ./task_support/make_input .task-build/input-now.mp4
test -s .task-build/input-now.mp4
.task-build/gap-example .task-build/input-now.mp4 .task-build/example-now.mp4
test -s .task-build/example-now.mp4
.task-build/api-normal .task-build/example-now.mp4
.task-build/defrag-cli -preserve-gaps .task-build/input-now.mp4 .task-build/cli-now.mp4
test -s .task-build/cli-now.mp4
.task-build/api-normal .task-build/cli-now.mp4
go test -count=1 -v ./mp4 -run '^(TestGapIslands|TestGapEdits|TestGapRetransmit)$'
) >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in TestGapIslands TestGapEdits TestGapRetransmit; do
    grep -Eq "^=== RUN +${group}$" "$output_log"
    grep -Eq "^--- PASS: ${group} " "$output_log"
    if grep -Eq "^[[:space:]]*--- (SKIP|FAIL): ${group}([/ ]|$)" "$output_log"; then exit 4; fi
    grep -Eq "TASK_GROUP_DONE ${group} checks=[1-9][0-9]*$" "$output_log"
  done
  grep -qx 'PASS' "$output_log"
fi
if [[ $mode == baseline ]]; then go run ./task_support/baseline; fi
