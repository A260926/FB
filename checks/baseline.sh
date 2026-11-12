#!/bin/bash
set -euo pipefail
build=$(mktemp -d)
trap 'rm -rf "$build"' EXIT
cc -std=c99 -O1 -I. schrift.c $(test ! -f schrift_cff.c || printf schrift_cff.c) checks/baseline.c -lm -o "$build/base"
"$build/base"
node checks/make_cff.mjs "$build/cubic.otf"
echo SCHRIFT_FIXTURE_GENERATOR_OK
