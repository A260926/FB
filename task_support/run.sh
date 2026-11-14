#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
mkdir -p .task-build/tmp
export TMPDIR="$PWD/.task-build/tmp"
export GOPROXY=off GOSUMDB=off GOTOOLCHAIN=local GOFLAGS=-mod=vendor
export GOMODCACHE="$(mktemp -d "$TMPDIR/mod.XXXXXX")" GOCACHE="$(mktemp -d "$TMPDIR/build.XXXXXX")"
if [[ $mode == target ]]; then
  test -s edit_assembly_test.go
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  go test -v -count=1 . -run '^(TestEditAssemblyTimeline|TestEditAssemblyWords|TestEditAssemblyExport)$' >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in TestEditAssemblyTimeline TestEditAssemblyWords TestEditAssemblyExport; do
    grep -Eq "^=== RUN +${group}$" "$output_log"
    grep -Eq "^--- PASS: ${group} " "$output_log"
    if grep -Eq "^[[:space:]]*--- (SKIP|FAIL): ${group}([/ ]|$)" "$output_log"; then exit 4; fi
    grep -Eq "TASK_GROUP_DONE ${group} checks=[1-9][0-9]*$" "$output_log"
  done
  grep -qx 'PASS' "$output_log"
fi
go test -v -count=1 . -run '^(TestSeedEditBaseline|TestSubtitles_Fragment|TestSubtitles_ApplyLinearCorrection|TestWebVTT|TestParseTextWebVTT|TestTTML)$'
