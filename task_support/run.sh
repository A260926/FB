#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
mkdir -p .task-build
task_go_cache=$(mktemp -d)
task_mod_cache=$(mktemp -d)
trap 'rm -rf "$task_go_cache" "$task_mod_cache"' EXIT
export GOCACHE="$task_go_cache" GOMODCACHE="$task_mod_cache" GOPROXY=off GOSUMDB=off GOTOOLCHAIN=local GOFLAGS=-mod=mod
go build -o .task-build/baseline ./task_support/baseline
if [[ $mode == target ]]; then
  test -s render/reconnect_test.go
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  (
set -e
go list -f '{{range .GoFiles}}{{println .}}{{end}}' ./render > .task-build/required-go-files.list
grep -qx 'instance_scene.go' .task-build/required-go-files.list
grep -qx 'reconnect.go' .task-build/required-go-files.list
test -s render/instance_scene.go
test -s render/reconnect.go
test -s examples/reconnect/main.go
go build ./render
go build -o .task-build/reconnect ./examples/reconnect
rm -f .task-build/reconnect-output/before.png .task-build/reconnect-output/after.png
mkdir -p .task-build/reconnect-output
.task-build/reconnect .task-build/reconnect-output
test -s .task-build/reconnect-output/before.png
test -s .task-build/reconnect-output/after.png
go run ./task_support/check_images .task-build/reconnect-output
go test ./render -run '^(TestInstanceMeasure|TestProposalMixture|TestReconnectTransport)$' -v -count=1
) >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in TestInstanceMeasure TestProposalMixture TestReconnectTransport; do
    grep -Eq "^=== RUN +${group}$" "$output_log"
    grep -Eq "^--- PASS: ${group} " "$output_log"
    if grep -Eq "^[[:space:]]*--- (SKIP|FAIL): ${group}([/ ]|$)" "$output_log"; then exit 4; fi
    grep -Eq "TASK_GROUP_DONE ${group} checks=[1-9][0-9]*$" "$output_log"
  done
  grep -qx 'PASS' "$output_log"
fi
.task-build/baseline
