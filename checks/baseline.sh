#!/bin/bash
set -euo pipefail
build=$(mktemp -d)
trap 'rm -rf "$build"' EXIT
cd libmspack
cc -std=c99 -O2 -DHAVE_INTTYPES_H=1 -DSIZEOF_OFF_T=8 -DTEST_FILES=test/test_files/cabd -I mspack -I test test/cabd_test.c test/md5.c mspack/system.c mspack/cabd.c mspack/lzxd.c mspack/mszipd.c mspack/qtmd.c -o "$build/cabd_test"
"$build/cabd_test" 2> "$build/warnings.log" | tee "$build/old.log"
grep -Fx 'ALL 433 TESTS PASSED.' "$build/old.log"
echo CAB_BASELINE_OK
