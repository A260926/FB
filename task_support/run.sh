#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
mkdir -p .task-build/gocache .task-build/modcache
export GOCACHE="$PWD/.task-build/gocache" GOMODCACHE="$PWD/.task-build/modcache"
export GOPROXY=off GOTOOLCHAIN=local GOWORK=off GOFLAGS=-mod=vendor
if [[ $mode == target ]]; then
  test -s internal/gotestrunner/fuzzcontrol_contract_test.go
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  (
set -e
for task_file in internal/gotestrunner/fuzzcontrol.go internal/gotestrunner/fuzzoutput.go cmd/fuzzsession_demo/main.go; do test -s "$task_file"; done
go list -f '{{join .GoFiles "\n"}}' ./internal/gotestrunner > .task-build/production-files.txt
grep -qx fuzzcontrol.go .task-build/production-files.txt
grep -qx fuzzoutput.go .task-build/production-files.txt
go run ./task_support/api_consumer
go build -o .task-build/fuzzsession-demo ./cmd/fuzzsession_demo
.task-build/fuzzsession-demo > .task-build/demo.txt
grep -qx 'fuzz-session generations=1 exit=0' .task-build/demo.txt
go test -count=1 -v ./internal/gotestrunner -run '^(TestFuzzReplacement|TestFuzzCancellation|TestFuzzAttribution)$'
) >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in TestFuzzReplacement TestFuzzCancellation TestFuzzAttribution; do
    grep -Eq "^=== RUN +${group}$" "$output_log"
    grep -Eq "^--- PASS: ${group} " "$output_log"
    if grep -Eq "^[[:space:]]*--- (SKIP|FAIL): ${group}([/ ]|$)" "$output_log"; then exit 4; fi
    grep -Eq "TASK_GROUP_DONE ${group} checks=[1-9][0-9]*$" "$output_log"
  done
  grep -qx 'PASS' "$output_log"
fi
go run ./task_support/baseline
