#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
mkdir -p .task-build
cache_dir=$(mktemp -d)
export GOMODCACHE="$cache_dir/mod" GOCACHE="$cache_dir/build" GOPROXY=off GOTOOLCHAIN=local GOSUMDB=off
trap 'rm -rf "$cache_dir"' EXIT
go build ./trace ./scene ./xmath ./colour
if [[ $mode == target ]]; then
  test -s trace/live_scene_test.go
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  (
set -e
test -s trace/live_scene.go
test -s examples/live_scene/main.go
go build -o .task-build/live-scene ./examples/live_scene
rm -f .task-build/live-scene.png
.task-build/live-scene .task-build/live-scene.png
test -s .task-build/live-scene.png
go test -race -count=1 -v ./trace -run '^(TestLiveGeometry|TestLivePublish|TestLiveWorkers)$'
) >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in TestLiveGeometry TestLivePublish TestLiveWorkers; do
    grep -Eq "^=== RUN +${group}$" "$output_log"
    grep -Eq "^--- PASS: ${group} " "$output_log"
    if grep -Eq "^[[:space:]]*--- (SKIP|FAIL): ${group}([/ ]|$)" "$output_log"; then exit 4; fi
    grep -Eq "TASK_GROUP_DONE ${group} checks=[1-9][0-9]*$" "$output_log"
  done
  grep -qx 'PASS' "$output_log"
fi
go test -count=1 -v ./trace -run '^TestBaseline$'
