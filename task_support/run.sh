#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
build=$(mktemp -d)
trap 'rm -rf "$build"' EXIT
build_case() { cc -std=c99 -O1 -Iinclude -Isrc src/*.c "$1" -o "$2"; }
build_target() { cc -std=c99 -O1 -Iinclude -Isrc -I. src/*.c task_support/main.c tests/task_cases.c -o "$build/target"; }
if [[ "$mode" == baseline ]]; then
  build_case task_support/baseline.c "$build/baseline"
  "$build/baseline"
elif [[ "$mode" == target ]]; then
  test -s tests/task_cases.c || { echo 'missing tests/task_cases.c' >&2; exit 2; }
  build_target
  "$build/target"
  build_case task_support/baseline.c "$build/baseline"
  "$build/baseline"
else
  exit 2
fi
