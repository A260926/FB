#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
mkdir -p .task-build
task_cache=$(mktemp -d "${TMPDIR:-/tmp}/go-cache.XXXXXX")
export GOCACHE="$task_cache/build" GOMODCACHE="$task_cache/modules" GOPROXY=off GOSUMDB=off GOTOOLCHAIN=local
mkdir -p "$GOCACHE" "$GOMODCACHE"
trap 'rm -rf "$task_cache"' EXIT
if [[ $mode == target ]]; then
  test -s terrain_target_test.go
  output_log=$(mktemp)
  trap 'rm -f "$output_log"; rm -rf "$task_cache"' EXIT
  set +e
  bash task_support/target-command.sh >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in TestTerrainSelection TestTerrainSeams TestTerrainUpdates; do
    grep -Eq "^=== RUN +${group}$" "$output_log"
    grep -Eq "^--- PASS: ${group} " "$output_log"
    if grep -Eq "^[[:space:]]*--- (SKIP|FAIL): ${group}([/ ]|$)" "$output_log"; then exit 4; fi
    grep -Eq "TASK_GROUP_DONE ${group} checks=[1-9][0-9]*$" "$output_log"
  done
  grep -qx 'PASS' "$output_log"
fi
go test -v -count=1 -timeout=60s -run '^TestBaselineRender$' .
