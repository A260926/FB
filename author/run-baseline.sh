#!/bin/sh
set -eu
build=$(mktemp -d)
trap 'rm -rf "$build"' EXIT HUP INT TERM
cc -std=c99 -O2 -Iinclude src/*.c author/baseline.c -lm -o "$build/baseline"
"$build/baseline"
