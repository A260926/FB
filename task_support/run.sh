#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
mkdir -p .task-build
cache_root=$(mktemp -d)
trap 'rm -rf "$cache_root"' EXIT
export GOMODCACHE="$cache_root/mod" GOCACHE="$cache_root/cache" GOPROXY=off GOTOOLCHAIN=local GOFLAGS=-mod=vendor
go build ./mp4
if [[ $mode == target ]]; then
  test -s mp4/edit_compact_test.go
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  (
set -e
for src in mp4/edit_compact.go mp4/compact_tables.go examples/editcompact/main.go; do test -s "$src"; done
active_files=$(go list -f '{{range .GoFiles}}{{println .}}{{end}}' ./mp4)
for src in edit_compact.go compact_tables.go; do printf '%s\n' "$active_files" | grep -Fx "$src"; done
go build -o .task-build/editcompact ./examples/editcompact
rm -f .task-build/compact.mp4
.task-build/editcompact task_support/edited_tracks.mp4 .task-build/compact.mp4
test -s .task-build/compact.mp4
go test -count=1 -v ./mp4 -run '^(TestCompactReachability|TestCompactRoundTrip|TestCompactReject)$'
) >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in TestCompactReachability TestCompactRoundTrip TestCompactReject; do
    grep -Eq "^=== RUN +${group}$" "$output_log"
    grep -Eq "^--- PASS: ${group} " "$output_log"
    if grep -Eq "^[[:space:]]*--- (SKIP|FAIL): ${group}([/ ]|$)" "$output_log"; then exit 4; fi
    grep -Eq "TASK_GROUP_DONE ${group} checks=[1-9][0-9]*$" "$output_log"
  done
  grep -qx 'PASS' "$output_log"
fi
go run ./task_support/baseline
