#!/bin/sh
set -eu
build=$(mktemp -d)
trap 'rm -rf "$build"' EXIT HUP INT TERM
${CC:-cc} -std=c99 -O2 -I. checks/baseline.c -o "$build/baseline"
"$build/baseline"
${CC:-cc} -std=c99 -O2 -I. checks/search-witness.c -o "$build/search-witness"
"$build/search-witness"
