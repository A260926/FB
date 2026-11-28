#!/bin/bash
set -euo pipefail
# Test observation points are read-only and only exist when explicitly
# requested; the fixed driver build turns them on so the checks can inspect
# the transform and quantizer internals. Normal library builds never see them.
extra=${SHINE_TEST_HOOKS:+-DSHINE_TEST_HOOKS}
for source in src/lib/*.c; do
 name=${source##*/}
 cc -std=c99 -O2 -Isrc/lib $extra -c "$source" -o "$1/${name%.c}.o"
done
ar rcs "$1/libshine.a" "$1/"*.o
