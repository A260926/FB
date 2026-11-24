#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
rm -rf .task-go
mkdir -p .task-go/mod .task-go/cache .task-go/path
export GOENV=off GOWORK=off GOPROXY=off GOSUMDB=off GOTOOLCHAIN=local
export GOMODCACHE="$PWD/.task-go/mod" GOCACHE="$PWD/.task-go/cache" GOPATH="$PWD/.task-go/path"
if [[ $mode == target ]]; then
  test -s commands/differential.go
  test -s commands/diff_values.go
  test -s cmd/handlediff/main.go
  for production_file in differential.go diff_values.go; do
    go list -f '{{range .GoFiles}}{{println .}}{{end}}' ./commands | grep -qx "$production_file"
  done
fi
if [[ $mode == target ]]; then
  test -s commands/differential_contract_test.go
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  (
set -e
go run ./task_support/apiprobe
go test -count=1 -v -timeout=60s -run '^TestGeneratedProperty$' ./task_support/apiprobe
test -s cmd/handlediff/main.go
rm -f .task-go/demo.out
go run ./cmd/handlediff > .task-go/demo.out
grep -qx 'equal handles=2' .task-go/demo.out
cat .task-go/demo.out
go test -count=1 -v -timeout=90s -run '^(TestDiffHandles|TestDiffTransactions|TestDiffCandidates)$' ./commands
) >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in TestDiffHandles TestDiffTransactions TestDiffCandidates; do
    grep -Eq "^=== RUN +${group}$" "$output_log"
    grep -Eq "^--- PASS: ${group} " "$output_log"
    if grep -Eq "^[[:space:]]*--- (SKIP|FAIL): ${group}([/ ]|$)" "$output_log"; then exit 4; fi
    grep -Eq "TASK_GROUP_DONE ${group} checks=[1-9][0-9]*$" "$output_log"
  done
  grep -qx 'PASS' "$output_log"
fi
go test -count=1 -v -timeout=90s ./commands
