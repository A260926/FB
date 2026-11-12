#!/bin/sh
set -eu
build=$(mktemp -d)
trap 'rm -rf "$build"' EXIT HUP INT TERM
${CC:-cc} -std=c11 -O2 -Ilibfaac checks/baseline.c libfaac/huffdata.c libfaac/bitstream.c libfaac/util.c -lm -o "$build/baseline"
"$build/baseline" "$@"
