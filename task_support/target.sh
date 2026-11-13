#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
test -s task_tests/target.cpp
build=$(mktemp -d)
trap 'rm -rf "$build"' EXIT
bash task_support/build.sh target "$build"
set +e
"$build/target" geometry seams compositing ownership >"$build/stdout" 2>"$build/stderr"
status=$?
set -e
cat "$build/stdout"
cat "$build/stderr" >&2
if [ "$status" -ne 0 ]; then exit "$status"; fi
test "$(grep -Ec '^GROUP geometry cases=[1-9][0-9]* checks=[1-9][0-9]* DONE$' "$build/stdout")" -eq 1
test "$(grep -Ec '^GROUP seams cases=[1-9][0-9]* checks=[1-9][0-9]* DONE$' "$build/stdout")" -eq 1
test "$(grep -Ec '^GROUP compositing cases=[1-9][0-9]* checks=[1-9][0-9]* DONE$' "$build/stdout")" -eq 1
test "$(grep -Ec '^GROUP ownership cases=[1-9][0-9]* checks=[1-9][0-9]* DONE$' "$build/stdout")" -eq 1
test "$(grep -c '^TASK_ALL_DONE$' "$build/stdout")" -eq 1
bash task_support/baseline.sh
