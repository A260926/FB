#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
build=$(mktemp -d "$PWD/.geometry-build.XXXXXX")
export TMPDIR="$build"
trap 'rm -rf "$build"' EXIT
case "${1:-baseline}" in
baseline)
  ${CC:-cc} -std=c99 -O1 -ffp-contract=off -I. support/baseline.c -lm -o "$build/baseline"
  "$build/baseline"
  ${CXX:-c++} -std=c++17 -O1 -ffp-contract=off -I. support/old.cpp -lm -o "$build/old"
  "$build/old"
  ;;
target)
  test -s testing/geometry_contract.cpp
  test -s examples/geometry_ribbon.c
  ${CXX:-c++} -std=c++17 -O1 -DPGL_CHUNK_PRIMS=64 -ffp-contract=off -I. -Isupport support/driver.cpp testing/geometry_contract.cpp -lm -o "$build/target"
  ${CC:-cc} -std=c99 -O1 -ffp-contract=off -I. examples/geometry_ribbon.c -lm -o "$build/example"
  ${CC:-cc} -std=c99 -O1 support/check_ppm.c -o "$build/check-ppm"
  mkdir -p .target-results
  set +e
  "$build/target" > .target-results/stdout.txt 2> .target-results/stderr.txt
  status=$?
  set -e
  printf '%s\n' "$status" > .target-results/exit-status.txt
  cat .target-results/stdout.txt
  cat .target-results/stderr.txt >&2
  if [ "$status" -ne 0 ]; then exit "$status"; fi
  awk -f support/verify-target.awk .target-results/stdout.txt
  "$build/example" "$build/ribbon.ppm"
  "$build/check-ppm" "$build/ribbon.ppm"
  bash support/run.sh baseline
  ;;
*) exit 2;;
esac
