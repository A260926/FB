#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
mkdir -p .task-build/gocache .task-build/modcache
export GOENV=off GOTOOLCHAIN=local GOTELEMETRY=off GOWORK=off GOPROXY=off GOSUMDB=off GOFLAGS=-mod=vendor
export GOCACHE="$PWD/.task-build/gocache" GOMODCACHE="$PWD/.task-build/modcache"
go build ./pkg/flexfec
if [[ $mode == target ]]; then
  test -s pkg/flexfec/task_repair_receiver_test.go
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  (
set -e
test -s pkg/flexfec/receiver_interceptor.go
go list -f '{{range .GoFiles}}{{println .}}{{end}}' ./pkg/flexfec | grep -qx receiver_interceptor.go
test -s examples/flexfec-receive/main.go
rm -f .task-build/flexfec-receive-example
go build -o .task-build/flexfec-receive-example ./examples/flexfec-receive
.task-build/flexfec-receive-example
go test -count=1 -timeout=90s -v ./pkg/flexfec -run '^(TestRepairRank|TestRepairWindow|TestRepairIngress)$'
) >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in TestRepairRank TestRepairWindow TestRepairIngress; do
    grep -Eq "^=== RUN +${group}$" "$output_log"
    grep -Eq "^--- PASS: ${group} " "$output_log"
    if grep -Eq "^[[:space:]]*--- (SKIP|FAIL): ${group}([/ ]|$)" "$output_log"; then exit 4; fi
    grep -Eq "TASK_GROUP_DONE ${group} checks=[1-9][0-9]*$" "$output_log"
  done
  grep -qx 'PASS' "$output_log"
fi
if [[ $mode == baseline ]]; then
  go test -count=1 -timeout=90s -tags baseline_gap -v ./pkg/flexfec -run '^(TestFlexFec03_SimpleRoundTrip|TestRankWitness)$'
else
  go test -count=1 -timeout=90s -v ./pkg/flexfec -run '^TestFlexFec03_SimpleRoundTrip$'
fi
