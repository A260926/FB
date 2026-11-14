#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
export GOPROXY=off GOSUMDB=off GOTOOLCHAIN=local GOWORK=off GOFLAGS=-mod=vendor
export GOMODCACHE="$PWD/.task-build/modcache" GOCACHE="$PWD/.task-build/cache"
mkdir -p "$GOMODCACHE" "$GOCACHE"
if [[ $mode == target ]]; then
  test -s batch_context_test.go
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  go test -mod=vendor -vet=off -v -count=1 -timeout=45s . -run '^(TestBatchContextBoundary|TestBatchContextBuffer|TestBatchContextSession)$' >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in TestBatchContextBoundary TestBatchContextBuffer TestBatchContextSession; do
    grep -Eq "^=== RUN +${group}$" "$output_log"
    grep -Eq "^--- PASS: ${group} " "$output_log"
    if grep -Eq "^[[:space:]]*--- (SKIP|FAIL): ${group}([/ ]|$)" "$output_log"; then exit 4; fi
    grep -Eq "TASK_GROUP_DONE ${group} checks=[1-9][0-9]*$" "$output_log"
  done
  grep -qx 'PASS' "$output_log"
fi
go run -mod=vendor ./task_support/baseline
go test -mod=vendor -vet=off -v -count=1 -timeout=45s . -run '^(TestBatcher|TestSpawnGeneric|TestOptions)$'
echo BASELINE_OK
