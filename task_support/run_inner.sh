#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
export GOPROXY=off GOSUMDB=off GOTOOLCHAIN=local CGO_ENABLED=0 GOFLAGS=-mod=vendor
mkdir -p .task-build
export GOPATH="$PWD/.task-build/gopath" GOMODCACHE="$PWD/.task-build/mod" GOCACHE="$PWD/.task-build/cache"
if [[ $mode == target ]]; then
  test -s frame_collect_test.go
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  go test -v -count=1 -run '^(TestFrameSources|TestFrameIdentity|TestFrameLifecycle)$' . >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in TestFrameSources TestFrameIdentity TestFrameLifecycle; do
    grep -Eq "^=== RUN +${group}$" "$output_log"
    grep -Eq "^--- PASS: ${group} " "$output_log"
    if grep -Eq "^--- (SKIP|FAIL): ${group} " "$output_log"; then exit 4; fi
  done
  grep -qx 'PASS' "$output_log"
fi
go test -v -count=1 -run '^(TestAcquisitionBaseline|TestFind.*|TestText|TestFullText.*|TestNewErrorReturnsInspectableError|TestHTML)$' .
