#!/usr/bin/env bash
set -eu
cd "$(dirname "$0")/.."
mode=${1:-baseline}
build=$(mktemp -d "${TMPDIR:-/tmp}/check-build.XXXXXX")
trap 'rm -rf "$build"' EXIT
find dex-reader-api/src/main/java dex-reader/src/main/java task_support -name '*.java' -type f > "$build/sources"
if [ "$mode" = target ]; then
  target=dex-reader/src/test/java/com/googlecode/d2j/reader/test/CompactDexRegression.java
  test -s "$target" || { echo "Missing nonempty $target" >&2; exit 2; }
  printf '%s\n' "$target" >> "$build/sources"
elif [ "$mode" != baseline ]; then
  echo "usage: bash task_support/run.sh baseline|target" >&2; exit 2
fi
javac -encoding UTF-8 -d "$build/classes" @"$build/sources"
if [ "$mode" = baseline ]; then
  java -cp "$build/classes" task_support.Observe
else
  java -cp "$build/classes" task_support.Observe normal
  java -cp "$build/classes" task_support.Check com.googlecode.d2j.reader.test.CompactDexRegression
fi
