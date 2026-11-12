#!/bin/sh
set -eu
build=$(mktemp -d)
trap 'rm -r "$build"' EXIT
cc -std=c99 -O2 regress/baseline.c -lm -o "$build/baseline"
"$build/baseline"
