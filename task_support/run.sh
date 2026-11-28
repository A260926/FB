#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
mkdir -p .task-build/modcache .task-build/gocache
export GOMODCACHE="$PWD/.task-build/modcache" GOCACHE="$PWD/.task-build/gocache" GOPROXY=off GOSUMDB=off GOTOOLCHAIN=local
go build -o .task-build/baseline ./task_support/baseline
if [[ $mode == target ]]; then
  test -s atlas_target_test.go
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  (
set -e
test -s atlas_capture.go
test -s atlas_reconstruct.go
go list -f '{{range .GoFiles}}{{println .}}{{end}}' . > .task-build/go-files.txt
grep -qx 'atlas_capture.go' .task-build/go-files.txt
grep -qx 'atlas_reconstruct.go' .task-build/go-files.txt
go run ./task_support/api_probe
test -s examples/atlas_reconstruct/main.go
example_dir=$(mktemp -d "$PWD/.task-build/atlas-example.XXXXXX")
go run ./examples/atlas_reconstruct "$example_dir"
go run ./task_support/png_check "$example_dir/average.png" "$example_dir/reconstructed.png" "$example_dir/average_model.png" "$example_dir/reconstructed_model.png"
go test -count=1 -v -run '^(TestAtlasVisibility|TestAtlasSeams|TestAtlasSolve)$' .
) >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in TestAtlasVisibility TestAtlasSeams TestAtlasSolve; do
    grep -Eq "^=== RUN +${group}$" "$output_log"
    grep -Eq "^--- PASS: ${group} " "$output_log"
    if grep -Eq "^[[:space:]]*--- (SKIP|FAIL): ${group}([/ ]|$)" "$output_log"; then exit 4; fi
    grep -Eq "TASK_GROUP_DONE ${group} checks=[1-9][0-9]*$" "$output_log"
  done
  grep -qx 'PASS' "$output_log"
fi
.task-build/baseline
