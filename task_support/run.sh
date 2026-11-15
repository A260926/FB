#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
export GOPROXY=off GOSUMDB=off GOTOOLCHAIN=local GOFLAGS=-mod=vendor
cache_dir=$(mktemp -d)
export GOMODCACHE="$cache_dir/mod" GOCACHE="$cache_dir/build"
trap 'rm -rf "$cache_dir"' EXIT
mkdir -p .task-build
go build -o .task-build/baseline ./task_support/baseline
if [[ $mode == target ]]; then
  test -s render/process_test.go
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  (
set -e
test -s render/process_octree.go
test -s render/process_sampler.go
test -s cmd/processmesh/main.go
go build -o .task-build/processmesh ./cmd/processmesh
rm -f .task-build/process.stl
.task-build/processmesh -output .task-build/process.stl
test -s .task-build/process.stl
go test ./render -run '^TestProcess(Geometry|Window|Lifecycle)$' -count=1 -v -timeout=90s
) >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in TestProcessGeometry TestProcessWindow TestProcessLifecycle; do
    grep -Eq "^=== RUN +${group}$" "$output_log"
    grep -Eq "^--- PASS: ${group} " "$output_log"
    if grep -Eq "^[[:space:]]*--- (SKIP|FAIL): ${group}([/ ]|$)" "$output_log"; then exit 4; fi
    grep -Eq "TASK_GROUP_DONE ${group} checks=[1-9][0-9]*$" "$output_log"
  done
  grep -qx 'PASS' "$output_log"
fi
.task-build/baseline
