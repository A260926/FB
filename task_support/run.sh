#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
case "$mode" in baseline|target) ;; *) exit 2;; esac
if [[ "$mode" == target ]]; then test -s mesh/src/test/java/regression/OpenDooSabinChecks.java; fi
build=$(mktemp -d)
trap 'rm -rf "$build"' EXIT
find math/src/main/java mesh/src/main/java -name '*.java' -print >"$build/sources"
find task_support -name '*.java' -print >>"$build/sources"
if [[ "$mode" == target ]]; then printf '%s\n' mesh/src/test/java/regression/OpenDooSabinChecks.java >>"$build/sources"; fi
javac --release 17 -encoding UTF-8 -d "$build/classes" @"$build/sources"
if [[ "$mode" == baseline ]]; then
  java -cp "$build/classes" task_support.Baseline
else
  set +e
  java -cp "$build/classes" task_support.Suite >"$build/output" 2>&1
  status=$?
  set -e
  cat "$build/output"
  [[ "$status" == 0 ]] || exit "$status"
  for group in fans boundary repeat compat; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$build/output"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$build/output"
fi
