#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
mkdir -p .support-build
mode=${1:-baseline}
${CC:-cc} -std=c99 -O2 -Isrc -c src/tinyspline.c -o .support-build/tinyspline.o
${CC:-cc} -std=c99 -O2 -Isrc -c src/parson.c -o .support-build/parson.o
case "$mode" in
 baseline)
 ${CC:-cc} -std=c99 -O2 -Isrc support/baseline.c .support-build/tinyspline.o .support-build/parson.o -lm -o .support-build/check
 .support-build/check
 ${CC:-cc} -std=c99 -O2 -Isrc -Itest -Itest/cutest test/c/*.c test/testutils.c test/cutest/CuTest.c .support-build/tinyspline.o .support-build/parson.o -lm -o .support-build/upstream
 .support-build/upstream
 ;;
 target)
 test -f tests/knot_removal.c; test -f src/tinyspline_reduce.c
 ${CC:-cc} -std=c99 -O2 -Isrc tests/knot_removal.c src/tinyspline_reduce.c .support-build/tinyspline.o .support-build/parson.o -lm -o .support-build/target
 .support-build/target
 sh support/run.sh baseline
 ;;
 *) exit 2;;
esac
