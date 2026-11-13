#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
cc=${CC:-cc}
build=$(mktemp -d "${TMPDIR:-/tmp}/yara-hex-baseline.XXXXXX")
trap 'rm -rf "$build"' EXIT
for threshold in 200 2; do
 mkdir "$build/$threshold"
 bash task_support/build-library.sh "$build/$threshold" "$threshold"
 "$cc" -std=gnu11 -DHAVE_STDBOOL_H=1 -DYR_STRING_CHAINING_THRESHOLD="$threshold" -I libyara/include task_support/baseline.c "$build/$threshold/libyara.a" -pthread -lm -o "$build/$threshold/probe"
 "$build/$threshold/probe"
done
