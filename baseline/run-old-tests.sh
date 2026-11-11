#!/usr/bin/env bash
set -euo pipefail
b=.build/bin
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
for name in TestBufferOverrun::ctu_malloc TestBufferOverrun::ctu_array TestBufferOverrun::ctu_variable TestBufferOverrun::ctu_arithmetic TestNullPointer::ctuTest TestUninitVar::ctuTest; do
    "$b/testrunner" "$name" 2>&1 | tee "$work/test.log"
    grep -Fxq "$name" "$work/test.log"
    grep -Eq '^Number of tests: 1$' "$work/test.log"
    grep -Eq '^Tests failed: 0$' "$work/test.log"
done
printf '%s\n' 'CTU_OLD_TESTS_OK registered=6'
