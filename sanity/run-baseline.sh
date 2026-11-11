#!/bin/sh
set -eu
build=$(mktemp -d "${TMPDIR:-/tmp}/tgl-baseline.XXXXXX")
trap 'rm -rf "$build"' EXIT HUP INT TERM
cc -std=c99 -O2 -Iinclude src/*.c sanity/baseline.c -lm -o "$build/baseline"
"$build/baseline"
