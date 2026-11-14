#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
case $mode in baseline|target) ;; *) exit 2;; esac
if [[ $mode == target ]]; then
 test -s pkg/wasm/wasmfile/exception_flow.go || { echo "Missing nonempty production exception_flow.go" >&2; exit 2; }
 test -s examples/exception-flow/main.go || { echo "Missing nonempty exception-flow example" >&2; exit 2; }
fi
task_cache=$(mktemp -d "${TMPDIR:-/tmp}/check-cache.XXXXXX")
trap 'rm -rf "$task_cache"' EXIT
export GOCACHE="$task_cache/build" GOMODCACHE="$task_cache/modules" GOPROXY=off GOTOOLCHAIN=local GOSUMDB=off GOWORK=off
mkdir -p "$GOCACHE" "$GOMODCACHE"
go build -o "$task_cache/baseline" ./task_support/baseline
if [[ $mode == target ]]; then go build -o "$task_cache/example" ./examples/exception-flow; fi
if [[ $mode == target ]]; then
  test -s pkg/wasm/wasmfile/exception_flow_test.go
  output_log=$(mktemp)
  trap 'rm -f "$output_log"; rm -rf "$task_cache"' EXIT
  set +e
  go test -count=1 -v ./pkg/wasm/wasmfile >"$output_log" 2>&1
  task_test_rc=$?
  set -e
  cat "$output_log"
  [[ $task_test_rc == 0 ]] || exit "$task_test_rc"
  for group in TestExceptionDecode TestExceptionHandlers TestExceptionRecursion; do
    grep -Eq "^=== RUN +${group}$" "$output_log"
    grep -Eq "^--- PASS: ${group} " "$output_log"
    if grep -Eq "^[[:space:]]*--- (SKIP|FAIL): ${group}([/ ]|$)" "$output_log"; then exit 4; fi
    grep -Eq "TASK_GROUP_DONE ${group} checks=[1-9][0-9]*$" "$output_log"
  done
  grep -qx 'PASS' "$output_log"
fi
if [[ $mode == baseline ]]; then
 "$task_cache/baseline"
else
 "$task_cache/baseline" ordinary
 "$task_cache/example" task_support/fixtures/exception.wasm > "$task_cache/example.log"
 test -s "$task_cache/example.log"
 cat "$task_cache/example.log"
fi
