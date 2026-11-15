#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
mkdir -p .task-build
task_go_cache=$(mktemp -d .task-build/go-cache.XXXXXX)
export GOMODCACHE="$PWD/$task_go_cache/mod" GOCACHE="$PWD/$task_go_cache/build"
mkdir -p "$GOMODCACHE" "$GOCACHE"
export GOPROXY=off GOSUMDB=off GOTOOLCHAIN=local GOFLAGS=-mod=vendor
go build ./model3d
if [[ $mode == target ]]; then
  test -s model3d/dc_process_test.go
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  (
set -e
test -s model3d/dc_process.go
test -s cmd/dcprocess/main.go
go build -o .task-build/dcprocess ./cmd/dcprocess
rm -f .task-build/process.stl
.task-build/dcprocess -output .task-build/process.stl
test -s .task-build/process.stl
go test -count=1 -timeout=120s -v ./model3d -run '^(TestDCProcessGeometry|TestDCProcessRepair|TestDCProcessLifecycle)$'
) >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in TestDCProcessGeometry TestDCProcessRepair TestDCProcessLifecycle; do
    grep -Eq "^=== RUN +${group}$" "$output_log"
    grep -Eq "^--- PASS: ${group} " "$output_log"
    if grep -Eq "^[[:space:]]*--- (SKIP|FAIL): ${group}([/ ]|$)" "$output_log"; then exit 4; fi
    grep -Eq "TASK_GROUP_DONE ${group} checks=[1-9][0-9]*$" "$output_log"
  done
  grep -qx 'PASS' "$output_log"
fi
go run ./task_support/baseline
