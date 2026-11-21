#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
export GOPROXY=off GOSUMDB=off GOTOOLCHAIN=local GOWORK=off
cache_root=$(mktemp -d)
trap 'rm -rf "$cache_root"' EXIT
export GOCACHE="$cache_root/build" GOMODCACHE="$cache_root/mod"
mkdir -p "$GOCACHE" "$GOMODCACHE"
go build .
if [[ $mode == target ]]; then
  test -s volume_adjoint_target_test.go
  output_log=$(mktemp)
  trap 'rm -f "$output_log"; rm -rf "$cache_root"' EXIT
  set +e
  (
set -e
test -s volume_capture.go
go list -f '{{range .GoFiles}}{{println .}}{{end}}' . | grep -qx 'volume_capture.go'
test -s volume_transport.go
go list -f '{{range .GoFiles}}{{println .}}{{end}}' . | grep -qx 'volume_transport.go'
test -s volume_adjoint.go
go list -f '{{range .GoFiles}}{{println .}}{{end}}' . | grep -qx 'volume_adjoint.go'
go build .
test -s examples/volume_adjoint/main.go
mkdir -p .cache/volume_adjoint
rm -f .cache/volume_adjoint/beauty.ppm .cache/volume_adjoint/gradients.csv
go run ./examples/volume_adjoint .cache/volume_adjoint
test -s .cache/volume_adjoint/beauty.ppm
test -s .cache/volume_adjoint/gradients.csv
go test -count=1 -v . -run '^(TestVolumeCapture|TestVolumeAdjoint|TestVolumeRecord)$'
) >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in TestVolumeCapture TestVolumeAdjoint TestVolumeRecord; do
    grep -Eq "^=== RUN +${group}$" "$output_log"
    grep -Eq "^--- PASS: ${group} " "$output_log"
    if grep -Eq "^[[:space:]]*--- (SKIP|FAIL): ${group}([/ ]|$)" "$output_log"; then exit 4; fi
    grep -Eq "TASK_GROUP_DONE ${group} checks=[1-9][0-9]*$" "$output_log"
  done
  grep -qx 'PASS' "$output_log"
fi
if [[ $mode == baseline ]]; then go run ./task_support/baseline; fi
