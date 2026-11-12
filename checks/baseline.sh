#!/bin/sh
set -eu
build=$(mktemp -d)
trap 'rm -rf "$build"' EXIT HUP INT TERM
${CC:-cc} -std=c99 -O2 -I. checks/baseline.c g722_encode.c g722_decode.c -lm -o "$build/baseline"
"$build/baseline"
