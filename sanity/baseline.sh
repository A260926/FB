#!/usr/bin/env bash
set -euo pipefail
build=$(mktemp -d)
trap 'rm -rf "$build"' EXIT
cc -std=c99 -O2 -Isrc src/tinyspline.c src/parson.c sanity/baseline.c -lm -o "$build/baseline"
"$build/baseline"
cc -std=c99 -O2 -Isrc -Itest -Itest/cutest src/tinyspline.c src/parson.c test/cutest/CuTest.c test/testutils.c test/c/*.c -lm -o "$build/old-tests"
(cd test/c && "$build/old-tests")
echo 'BASELINE upstream C suite PASS'
