#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
. task_support/build_env.sh "$mode"
if [[ $mode == target ]]; then
  test -s go-fuzz/ownership_test.go
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  (
set -e
go test ./go-fuzz -run '^(TestOwnershipDebt|TestOwnershipLineage|TestOwnershipWorker|TestOwnershipAPIProbe)$' -count=1 -v
go build -o "$TASK_BUILD/go-fuzz" ./go-fuzz
example_dir=$(mktemp -d "$TASK_BUILD/example.XXXXXX")
"$TASK_BUILD/go-fuzz" -ownership-example="$example_dir"
test -s "$example_dir/trace.json"
go run ./task_support/checktrace "$example_dir/trace.json"
) >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in TestOwnershipDebt TestOwnershipLineage TestOwnershipWorker; do
    grep -Eq "^=== RUN +${group}$" "$output_log"
    grep -Eq "^--- PASS: ${group} " "$output_log"
    if grep -Eq "^[[:space:]]*--- (SKIP|FAIL): ${group}([/ ]|$)" "$output_log"; then exit 4; fi
    grep -Eq "TASK_GROUP_DONE ${group} checks=[1-9][0-9]*$" "$output_log"
  done
  grep -qx 'PASS' "$output_log"
fi
bash task_support/baseline.sh
