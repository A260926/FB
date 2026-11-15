#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
mkdir -p .task-build
cache_root=$(mktemp -d "${TMPDIR:-/tmp}/pt-cache.XXXXXX")
export GOCACHE="$cache_root/build" GOMODCACHE="$cache_root/mod" GOPATH="$cache_root/path"
mkdir -p "$GOCACHE" "$GOMODCACHE" "$GOPATH"
export GOPROXY=off GOSUMDB=off GOTOOLCHAIN=local GOFLAGS=-mod=readonly
export GONOSUMDB='*'
go build ./pt
if [[ $mode == target ]]; then
  test -s pt/linked_instance_test.go
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  (
set -e
test -s pt/linked_instance.go
test -s examples/linked_lights.go
go run examples/linked_lights.go .task-build/linked-lights.ppm
go test ./pt -run "^(TestLinkedInstances|TestLinkedLighting|TestLinkedUpdates)$" -count=1 -v
) >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in TestLinkedInstances TestLinkedLighting TestLinkedUpdates; do
    grep -Eq "^=== RUN +${group}$" "$output_log"
    grep -Eq "^--- PASS: ${group} " "$output_log"
    if grep -Eq "^[[:space:]]*--- (SKIP|FAIL): ${group}([/ ]|$)" "$output_log"; then exit 4; fi
    grep -Eq "TASK_GROUP_DONE ${group} checks=[1-9][0-9]*$" "$output_log"
  done
  grep -qx 'PASS' "$output_log"
fi
go test ./pt -run "^TestSeedLinkedBaseline$" -count=1 -v
