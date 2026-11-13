#!/bin/sh
set -eu
cc -std=c99 -UNDEBUG -Iexpat/lib -Ibuild support/baseline.c build/libexpat.a -o build/baseline
./build/baseline
set +e
./build/baseline --target-bound
witness_exit=$?
set -e
printf 'original_target_bound_exit=%s expected=1\n' "$witness_exit"
test "$witness_exit" -eq 1
ctest --test-dir build --output-on-failure
