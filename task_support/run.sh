#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
mkdir -p .task-build/go-cache .task-build/go-modcache
export GOPROXY=off GOSUMDB=off GOTOOLCHAIN=local
export GOCACHE="${TASK_GOCACHE:-$PWD/.task-build/go-cache}"
export GOMODCACHE="$PWD/.task-build/go-modcache"
if [[ $mode == target ]]; then
  test -s pkg/rawmessage/clock_wire_test.go
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  go test -mod=vendor -vet=off -v -count=1 ./pkg/rawmessage -run '^(TestClockWireWriter|TestClockWireReader|TestClockWireAck)$' >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in TestClockWireWriter TestClockWireReader TestClockWireAck; do
    grep -Eq "^=== RUN +${group}$" "$output_log"
    grep -Eq "^--- PASS: ${group} " "$output_log"
    if grep -Eq "^[[:space:]]*--- (SKIP|FAIL): ${group}([/ ]|$)" "$output_log"; then exit 4; fi
    grep -Eq "TASK_GROUP_DONE ${group} checks=[1-9][0-9]*$" "$output_log"
  done
  grep -qx 'PASS' "$output_log"
fi
go run -mod=vendor tools/baseline.go
go test -mod=vendor -vet=off -count=1 ./pkg/chunk ./pkg/rawmessage ./pkg/bytecounter -run '^(TestReader|TestReaderAdditional|TestReaderAbortChunkStream|TestWriter|TestWriterConcurrent|TestReaderAcknowledge|TestWriterAcknowledge|TestChunkRead|TestChunkMarshal|TestChunkMarshalHeaderTo)$'
echo RTMP_BASELINE_COMPLETE
