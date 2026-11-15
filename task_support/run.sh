#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
mkdir -p .task-build
export GOPROXY=off GOSUMDB=off GOTOOLCHAIN=local GOFLAGS=-mod=vendor
export GOMODCACHE="$PWD/.task-build/empty-modcache" GOCACHE="$PWD/.task-build/go-cache"
mkdir -p "$GOMODCACHE" "$GOCACHE"
go build ./ln
if [[ $mode == target ]]; then
  test -s ln/dynamic_tree_test.go
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  (
set -e
test -s ln/dynamic_tree.go
test -s examples/dynamic/main.go
rm -f .task-build/dynamic.svg
go run ./examples/dynamic .task-build/dynamic.svg
test -s .task-build/dynamic.svg
go test ./ln -run '^(TestMembership|TestTraversal|TestScene)$' -count=1 -v
) >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in TestMembership TestTraversal TestScene; do
    grep -Eq "^=== RUN +${group}$" "$output_log"
    grep -Eq "^--- PASS: ${group} " "$output_log"
    if grep -Eq "^[[:space:]]*--- (SKIP|FAIL): ${group}([/ ]|$)" "$output_log"; then exit 4; fi
    grep -Eq "TASK_GROUP_DONE ${group} checks=[1-9][0-9]*$" "$output_log"
  done
  grep -qx 'PASS' "$output_log"
fi
go run ./task_support/baseline
