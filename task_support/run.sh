#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
task_cache_root=$(mktemp -d)
output_log=''
task_cleanup() { rm -rf "$task_cache_root"; if [[ -n $output_log ]]; then rm -f "$output_log"; fi; }
trap task_cleanup EXIT
export GOMODCACHE="$task_cache_root/mod" GOCACHE="$task_cache_root/build" GOPATH="$task_cache_root/path" GOPROXY=off GOSUMDB=off GOTOOLCHAIN=local GOENV=off GOWORK=off
if [[ $mode == target ]]; then
  test -s analysis/linkmap_contract_test.go
  output_log=$(mktemp)
  trap task_cleanup EXIT
  test -s analysis/linkmap.go
  set +e
  go test -modfile=task_support/offline.mod -count=1 -v ./analysis -run '^(TestAliasResolution|TestTypeCompatibility|TestGlobalOffsets|TestResourceImage)$' >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in TestAliasResolution TestTypeCompatibility TestGlobalOffsets TestResourceImage; do
    grep -Eq "^=== RUN +${group}$" "$output_log"
    grep -Eq "^--- PASS: ${group} " "$output_log"
    if grep -Eq "^[[:space:]]*--- (SKIP|FAIL): ${group}([/ ]|$)" "$output_log"; then exit 4; fi
    grep -Eq "TASK_GROUP_DONE ${group} checks=[1-9][0-9]*$" "$output_log"
  done
  grep -qx 'PASS' "$output_log"
fi
go run -modfile=task_support/offline.mod ./task_support/baseline
