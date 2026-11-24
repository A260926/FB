#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
export GOENV=off GOTOOLCHAIN=local GOTELEMETRY=off GOWORK=off GOPROXY=off GOSUMDB=off GOFLAGS=-mod=vendor
export GOMODCACHE="$PWD/.build/gomodcache" GOCACHE="$PWD/.build/gocache" GOPATH="$PWD/.build/gopath"
mkdir -p "$GOMODCACHE" "$GOCACHE" "$GOPATH"
if [[ $mode == target ]]; then
  test -s pkg/jitterbuffer/redundant_test.go
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  (
set -e
test -s pkg/jitterbuffer/redundant.go
go list -f '{{range .GoFiles}}{{println .}}{{end}}' ./pkg/jitterbuffer | grep -qx redundant.go
go test -count=1 -v ./pkg/jitterbuffer -run '^(TestRedundantMerge|TestRedundantFeedback|TestRedundantLifecycle)$'
go run ./task_support/consumer
test -s examples/redundant/main.go
rm -f .build/redundant-example.out
go run ./examples/redundant > .build/redundant-example.out
test -s .build/redundant-example.out
cat .build/redundant-example.out
go run ./task_support/baseline
) >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in TestRedundantMerge TestRedundantFeedback TestRedundantLifecycle; do
    grep -Eq "^=== RUN +${group}$" "$output_log"
    grep -Eq "^--- PASS: ${group} " "$output_log"
    if grep -Eq "^[[:space:]]*--- (SKIP|FAIL): ${group}([/ ]|$)" "$output_log"; then exit 4; fi
    grep -Eq "TASK_GROUP_DONE ${group} checks=[1-9][0-9]*$" "$output_log"
  done
  grep -qx 'PASS' "$output_log"
fi
if [[ $mode == baseline ]]; then go run ./task_support/baseline; fi
