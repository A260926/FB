#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
mkdir -p .task-build/gomod .task-build/gocache
export GOMODCACHE="$PWD/.task-build/gomod" GOCACHE="$PWD/.task-build/gocache" GOPROXY=off GOTOOLCHAIN=local GOSUMDB=off

if [[ $mode == target ]]; then
  test -s internal/fuzz/replay_checkpoint_test.go
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  (
set -e
bash task_support/bind.sh
go test ./internal/fuzz -run '^(TestReplayBarrier|TestReplayFeedback|TestReplayRestart)$' -count=1 -v
) >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in TestReplayBarrier TestReplayFeedback TestReplayRestart; do
    grep -Eq "^=== RUN +${group}$" "$output_log"
    grep -Eq "^--- PASS: ${group} " "$output_log"
    if grep -Eq "^[[:space:]]*--- (SKIP|FAIL): ${group}([/ ]|$)" "$output_log"; then exit 4; fi
    grep -Eq "TASK_GROUP_DONE ${group} checks=[1-9][0-9]*$" "$output_log"
  done
  grep -qx 'PASS' "$output_log"
fi
go test ./internal/fuzz -run '^(TestProductionBaseline|TestQueue|TestByteRoundTrip|TestInt8RoundTrip)$' -count=1 -v
