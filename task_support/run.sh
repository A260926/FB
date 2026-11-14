#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s compact_unwind.go; test -s compact_unwind_contract_test.go; fi
task_cache_root=$(mktemp -d "${TMPDIR:-/tmp}/check-cache.XXXXXX")
trap 'rm -rf "$task_cache_root"; if [[ -n ${output_log:-} ]]; then rm -f "$output_log"; fi' EXIT
export GOMODCACHE="$task_cache_root/mod" GOCACHE="$task_cache_root/build"
export GOPROXY=off GOSUMDB=off GOTOOLCHAIN=local CGO_ENABLED=0 GOTELEMETRY=off GOFLAGS=-mod=vendor
mkdir -p "$GOMODCACHE" "$GOCACHE" .task-build
go build -o .task-build/baseline ./task_support/baseline
if [[ $mode == target ]]; then
  test -s compact_unwind_contract_test.go
  output_log=$(mktemp)

  set +e
  go test -v -count=1 . -run '^(TestCompactPages|TestCompactRecipes|TestCompactSources|TestNewFileSectionDataUsesReadAtAddr)$' >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in TestCompactPages TestCompactRecipes TestCompactSources; do
    grep -Eq "^=== RUN +${group}$" "$output_log"
    grep -Eq "^--- PASS: ${group} " "$output_log"
    if grep -Eq "^[[:space:]]*--- (SKIP|FAIL): ${group}([/ ]|$)" "$output_log"; then exit 4; fi
    grep -Eq "TASK_GROUP_DONE ${group} checks=[1-9][0-9]*$" "$output_log"
  done
  grep -qx 'PASS' "$output_log"
fi
.task-build/baseline
