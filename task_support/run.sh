#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
mkdir -p .task-build
task_go_cache=$(mktemp -d)
task_mod_cache=$(mktemp -d)
trap 'rm -rf "$task_go_cache" "$task_mod_cache"' EXIT
export GOCACHE="$task_go_cache" GOMODCACHE="$task_mod_cache" GOPROXY=off GOSUMDB=off GOTOOLCHAIN=local GOFLAGS=-mod=vendor
go build -o .task-build/baseline ./task_support/baseline
if [[ $mode == target ]]; then
  test -s ln/cage_mesh_test.go
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  (
set -e
go list -f '{{range .GoFiles}}{{println .}}{{end}}' ./ln > .task-build/required-go-files.list
grep -qx 'cage_binding.go' .task-build/required-go-files.list
grep -qx 'cage_mesh.go' .task-build/required-go-files.list
test -s ln/cage_binding.go
test -s ln/cage_mesh.go
test -s examples/cage/main.go
go build ./ln
go build -o .task-build/cage ./examples/cage
rm -f .task-build/cage.svg
.task-build/cage .task-build/cage.svg
test -s .task-build/cage.svg
grep -q '<svg' .task-build/cage.svg
go test ./ln -run '^(TestCageSplit|TestCagePose|TestCageScene)$' -v -count=1
) >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in TestCageSplit TestCagePose TestCageScene; do
    grep -Eq "^=== RUN +${group}$" "$output_log"
    grep -Eq "^--- PASS: ${group} " "$output_log"
    if grep -Eq "^[[:space:]]*--- (SKIP|FAIL): ${group}([/ ]|$)" "$output_log"; then exit 4; fi
    grep -Eq "TASK_GROUP_DONE ${group} checks=[1-9][0-9]*$" "$output_log"
  done
  grep -qx 'PASS' "$output_log"
fi
.task-build/baseline
