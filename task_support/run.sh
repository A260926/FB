#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
cache_dir=$(mktemp -d)
trap 'rm -rf "$cache_dir"' EXIT
export GOMODCACHE="$cache_dir/mod" GOCACHE="$cache_dir/build" GOPROXY=off GOTOOLCHAIN=local GOSUMDB=off
mkdir -p .task-build
if [[ $mode == target ]]; then
 test -s raytrace/hitables/patch_pages.go
 test -s examples/patch_pages/main.go
fi
if [[ $mode == target ]]; then
  test -s raytrace/hitables/patch_pages_test.go
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  (
set -e
go list -f '{{range .GoFiles}}{{println .}}{{end}}' ./raytrace/hitables > .task-build/required-go-files.list
grep -qx 'patch_pages.go' .task-build/required-go-files.list
go test -race ./raytrace/hitables -run '^$' -count=1
go build -o .task-build/patch-example ./examples/patch_pages
rm -f .task-build/before.png .task-build/after.png
.task-build/patch-example .task-build
test -s .task-build/before.png
test -s .task-build/after.png
! cmp -s .task-build/before.png .task-build/after.png
go test -race -v ./raytrace/hitables -run '^(TestPatchGeometry|TestPatchResidency|TestPatchUpdates)$' -count=1
) >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in TestPatchGeometry TestPatchResidency TestPatchUpdates; do
    grep -Eq "^=== RUN +${group}$" "$output_log"
    grep -Eq "^--- PASS: ${group} " "$output_log"
    if grep -Eq "^[[:space:]]*--- (SKIP|FAIL): ${group}([/ ]|$)" "$output_log"; then exit 4; fi
    grep -Eq "TASK_GROUP_DONE ${group} checks=[1-9][0-9]*$" "$output_log"
  done
  grep -qx 'PASS' "$output_log"
fi
go run ./task_support/baseline
