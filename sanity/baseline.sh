#!/usr/bin/env bash
set -euo pipefail
b=$(mktemp -d)
trap 'rm -rf "$b"' EXIT
cc -std=c99 -O0 -g -Isrc src/tinyspline.c src/parson.c sanity/rmf_baseline.c -lm -o "$b/witness"
"$b/witness"
cc -std=c99 -O0 -g -Isrc -Itest -Itest/cutest src/tinyspline.c src/parson.c test/cutest/CuTest.c test/testutils.c test/c/rmf.c sanity/rmf_old.c -lm -o "$b/old"
"$b/old"
