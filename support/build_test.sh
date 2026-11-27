#!/bin/bash
set -euo pipefail
# Test build: same sources as build.sh but with the read-only observation
# hooks enabled.  Used only by support/target.sh; a normal build (build.sh)
# has no hooks and no extra symbols.
for source in src/lib/*.c; do
 name=${source##*/}
 cc -std=c99 -O2 -DSHINE_TEST_HOOKS -Isrc/lib -c "$source" -o "$1/${name%.c}.o"
done
ar rcs "$1/libshine.a" "$1/"*.o
