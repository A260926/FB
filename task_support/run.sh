#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
mkdir -p .task-build
export GOTOOLCHAIN=local GOPROXY=off GOSUMDB=off CGO_ENABLED=0
export GOCACHE="${TASK_GOCACHE:-$PWD/.task-build/gocache}"
export GOMODCACHE="${TASK_GOMODCACHE:-$PWD/.task-build/gomodcache}"
export GOTMPDIR="$PWD/.task-build/gotmp"
export GOFLAGS='-mod=vendor -buildvcs=false'
mkdir -p "$GOCACHE" "$GOMODCACHE" "$GOTMPDIR"
go build -o .task-build/goresym .
if [[ $mode == target ]]; then
  test -s inline_contract_test.go
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  go test -v -count=1 -run '^(TestInlineChain|TestInlineCoordinates|TestInlineCLI)$' . >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in TestInlineChain TestInlineCoordinates TestInlineCLI; do
    grep -Eq "^=== RUN +${group}$" "$output_log"
    grep -Eq "^--- PASS: ${group} " "$output_log"
    if grep -Eq "^[[:space:]]*--- (SKIP|FAIL): ${group}([/ ]|$)" "$output_log"; then exit 4; fi
    grep -Eq "TASK_GROUP_DONE ${group} checks=[1-9][0-9]*$" "$output_log"
  done
  grep -qx 'PASS' "$output_log"
fi
bash task_support/fixtures.sh
go run ./task_support/baseline
go test -v -count=1 ./objfile ./objabi -run '^(TestVersionMapping|TestVersionMapping_Legacy|TestRtypeVersionMapping|TestReadPointer|TestReadSlice|TestMemoryReader|TestModuleDataIntermediate_FieldTypes|TestBinaryRegex|TestRegexGrouping|TestRegexpPatternFromYaraPattern|TestPathToPrefix)$'
echo BASELINE_OLD_LAYOUT_REGEX_PATH_OK
