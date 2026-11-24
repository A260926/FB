#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
export GOENV=off GOTOOLCHAIN=local GOTELEMETRY=off GOWORK=off GOPROXY=off GOSUMDB=off GOFLAGS=-mod=readonly
export GOMODCACHE="$PWD/.build/gomodcache" GOCACHE="$PWD/.build/gocache" GOPATH="$PWD/.build/gopath"
mkdir -p "$GOMODCACHE" "$GOCACHE" "$GOPATH"
if [[ $mode == target ]]; then
  test -s reload_sync_test.go
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  (
set -e
test -s reload_sync.go
go list -f '{{range .GoFiles}}{{println .}}{{end}}' . | grep -qx reload_sync.go
go test -count=1 -v . -run '^(TestReloadTags|TestReloadPlan|TestReloadCommit)$'
go run ./task_support/consumer
test -s examples/reload/main.go
rm -f .build/reload-example.out
go run ./examples/reload > .build/reload-example.out
test -s .build/reload-example.out
cat .build/reload-example.out
go run ./task_support/baseline
) >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in TestReloadTags TestReloadPlan TestReloadCommit; do
    grep -Eq "^=== RUN +${group}$" "$output_log"
    grep -Eq "^--- PASS: ${group} " "$output_log"
    if grep -Eq "^[[:space:]]*--- (SKIP|FAIL): ${group}([/ ]|$)" "$output_log"; then exit 4; fi
    grep -Eq "TASK_GROUP_DONE ${group} checks=[1-9][0-9]*$" "$output_log"
  done
  grep -qx 'PASS' "$output_log"
fi
if [[ $mode == baseline ]]; then go run ./task_support/baseline; fi
