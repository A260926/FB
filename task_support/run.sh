#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
mkdir -p .task-build
export GOMODCACHE="$PWD/.task-build/modcache"
export GOCACHE="$PWD/.task-build/gocache"
rm -rf "$GOMODCACHE" "$GOCACHE"
mkdir -p "$GOMODCACHE" "$GOCACHE"
export GOPROXY=off GOTOOLCHAIN=local GOWORK=off GOFLAGS=-mod=vendor
go build . ./wav ./effects ./generators
if [[ $mode == target ]]; then
  test -s timeline_target_test.go
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  (
set -e
test -s timeline.go
test -s timeline_plan.go
test -s examples/timeline/main.go
go list -f '{{range .GoFiles}}{{println .}}{{end}}' . > .task-build/production-files
for required in timeline.go timeline_plan.go; do
  if ! grep -qx "$required" .task-build/production-files; then echo "required production file excluded: $required"; exit 6; fi
done
go run ./task_support/api_probe
rm -f .task-build/timeline.wav
go run ./examples/timeline .task-build/timeline.wav
test -s .task-build/timeline.wav
go run ./task_support/check_wav .task-build/timeline.wav
go test . -run '^(TestTimelineRevision|TestTimelineOwnership|TestTimelineExport)$' -count=1 -v
) >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in TestTimelineRevision TestTimelineOwnership TestTimelineExport; do
    grep -Eq "^=== RUN +${group}$" "$output_log"
    grep -Eq "^--- PASS: ${group} " "$output_log"
    if grep -Eq "^[[:space:]]*--- (SKIP|FAIL): ${group}([/ ]|$)" "$output_log"; then exit 4; fi
    grep -Eq "TASK_GROUP_DONE ${group} checks=[1-9][0-9]*$" "$output_log"
  done
  grep -qx 'PASS' "$output_log"
fi
go run ./task_support/baseline
