#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
cache_root=$(mktemp -d)
trap 'rm -rf "$cache_root"' EXIT
export GOMODCACHE="$cache_root/modules" GOCACHE="$cache_root/build" GOPROXY=off GOSUMDB=off GOTOOLCHAIN=local
export GOFLAGS=-mod=mod
if [[ $mode == target ]]; then
  go build -o "$cache_root/expression-calls" ./examples/expression_calls
fi
if [[ $mode == target ]]; then
  test -s pkg/dwarf/godwarf/expression_calls_test.go
  output_log=$(mktemp)
  trap 'rm -f "$output_log"; rm -rf "$cache_root"' EXIT
  set +e
  go test -count=1 -v ./pkg/dwarf/godwarf -run '^(TestExpressionCallStack|TestExpressionCallUnits|TestExpressionCallErrors)$' >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in TestExpressionCallStack TestExpressionCallUnits TestExpressionCallErrors; do
    grep -Eq "^=== RUN +${group}$" "$output_log"
    grep -Eq "^--- PASS: ${group} " "$output_log"
    if grep -Eq "^[[:space:]]*--- (SKIP|FAIL): ${group}([/ ]|$)" "$output_log"; then exit 4; fi
    grep -Eq "TASK_GROUP_DONE ${group} checks=[1-9][0-9]*$" "$output_log"
  done
  grep -qx 'PASS' "$output_log"
fi
go test -count=1 ./pkg/dwarf ./pkg/dwarf/leb128 ./pkg/dwarf/op ./pkg/dwarf/godwarf
if [[ $mode == baseline ]]; then go run ./task_support/baseline; fi
if [[ $mode == target ]]; then
 go run ./task_support/baseline_fixture "$cache_root/sample.elf"
 "$cache_root/expression-calls" "$cache_root/sample.elf"
fi
