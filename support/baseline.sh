#!/bin/sh
# Reparse deferral workload check.
#
# Prerequisite: build/libexpat.a must have been configured with the
# scan probe enabled so that the unit_scan_work observation symbol
# exists, e.g.
#   cmake -S expat -B build -DEXPAT_SHARED_LIBS=OFF \
#         -DEXPAT_CHECK_SCAN_PROBE=ON
#   cmake --build build
#
# Before the fix, --target-bound exited 1 (quadratic re-scan work).
# With reparse deferral implemented it must exit 0 (linear bound held).
set -eu
cc -std=c99 -UNDEBUG -Iexpat/lib -Ibuild support/baseline.c build/libexpat.a -o build/baseline
./build/baseline
set +e
./build/baseline --target-bound
witness_exit=$?
set -e
printf 'target_linear_bound_exit=%s expected=0\n' "$witness_exit"
test "$witness_exit" -eq 0
ctest --test-dir build --output-on-failure
