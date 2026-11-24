#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
mkdir -p .task-build
rm -rf .task-build/gocache .task-build/modcache
mkdir -p .task-build/gocache .task-build/modcache
export GOMODCACHE="$PWD/.task-build/modcache" GOCACHE="$PWD/.task-build/gocache"
export GOPROXY=off GOSUMDB=off GOTOOLCHAIN=local CGO_ENABLED=0 GOFLAGS=-mod=vendor
if [[ $mode == baseline ]]; then go build -o .task-build/baseline ./task_support/baseline; fi
if [[ $mode == target ]]; then
  test -s internal/race/detector/task_replay_cases_test.go
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  (
set -e
test -s internal/race/detector/task_replay.go
test -s race/task_replay.go
test -s examples/task_replay/main.go
go list -f '{{range .GoFiles}}{{println .}}{{end}}' ./race > .task-build/public-files
grep -Fxq task_replay.go .task-build/public-files
go list -f '{{range .GoFiles}}{{println .}}{{end}}' ./internal/race/detector > .task-build/detector-files
grep -Fxq task_replay.go .task-build/detector-files
go run ./task_support/consumer
rm -f .task-build/task-replay-example.out
go run ./examples/task_replay > .task-build/task-replay-example.out
grep -qx TASK_REPLAY_EXAMPLE_OK .task-build/task-replay-example.out
cat .task-build/task-replay-example.out
go test ./internal/race/detector -count=1 -v -run '^(TestTaskMigration|TestTaskCancellation|TestTaskGeneration|TestTaskProvenance)$'
) >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in TestTaskMigration TestTaskCancellation TestTaskGeneration TestTaskProvenance; do
    grep -Eq "^=== RUN +${group}$" "$output_log"
    grep -Eq "^--- PASS: ${group} " "$output_log"
    if grep -Eq "^[[:space:]]*--- (SKIP|FAIL): ${group}([/ ]|$)" "$output_log"; then exit 4; fi
    grep -Eq "TASK_GROUP_DONE ${group} checks=[1-9][0-9]*$" "$output_log"
  done
  grep -qx 'PASS' "$output_log"
fi
if [[ $mode == baseline ]]; then .task-build/baseline; fi
