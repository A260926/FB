#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
mkdir -p .task-build/cache .task-build/mod .task-build/path
export GOTOOLCHAIN=local GOPROXY=off GOSUMDB=off
export GOCACHE="$PWD/.task-build/cache" GOMODCACHE="$PWD/.task-build/mod" GOPATH="$PWD/.task-build/path" GOFLAGS=-mod=vendor
if [[ $mode == target ]]; then
  test -s source_lifecycle_test.go
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  go test -race -v -count=1 -timeout=30s -run '^(TestSourceIsolation|TestSourceRetirement|TestSourceScheduling)$' . >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in TestSourceIsolation TestSourceRetirement TestSourceScheduling; do
    grep -Eq "^=== RUN +${group}$" "$output_log"
    grep -Eq "^--- PASS: ${group} " "$output_log"
    if grep -Eq "^[[:space:]]*--- (SKIP|FAIL): ${group}([/ ]|$)" "$output_log"; then exit 4; fi
    grep -Eq "TASK_GROUP_DONE ${group} checks=[1-9][0-9]*$" "$output_log"
  done
  grep -qx 'PASS' "$output_log"
fi
go test -race -v -count=1 -timeout=30s -run '^TestBaseline' .
go test -race -count=1 -timeout=30s ./client -run '^(TestSetDefaultHeader|TestConvertHeaderToMap|TestConvertMapToHeader|TestMeta|TestResponse_JoinURL)$'
go test -race -count=1 -timeout=30s ./middleware -run '^TestDuplicateRequests_ProcessRequest$'
