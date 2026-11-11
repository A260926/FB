#!/usr/bin/env bash
set -euo pipefail
b=$(mktemp -d)
trap 'rm -rf "$b"' EXIT
# C core, C++ wrapper, and the check driver.
cc -std=c99 -O0 -g -Isrc -c src/tinyspline.c -o "$b/tinyspline.o"
cc -std=c99 -O0 -g -Isrc -c src/parson.c -o "$b/parson.o"
c++ -std=c++11 -O0 -g -Isrc -c src/tinysplinecxx.cxx -o "$b/tinysplinecxx.o"
c++ -std=c++11 -O0 -g -Isrc sanity/rmf_check.cxx \
    "$b/tinyspline.o" "$b/parson.o" "$b/tinysplinecxx.o" \
    -lm -o "$b/rmf_check"
"$b/rmf_check"
# The original RMF unit test must keep passing.
cc -std=c99 -O0 -g -Isrc -Itest -Itest/cutest \
   src/tinyspline.c src/parson.c test/cutest/CuTest.c test/testutils.c \
   test/c/rmf.c sanity/rmf_old.c -lm -o "$b/rmf_old"
"$b/rmf_old"
