#!/bin/bash
set -euo pipefail
build=$(mktemp -d)
trap 'rm -rf "$build"' EXIT
cc -std=c99 -O2 -DHAVE_INTTYPES_H=1 -DSIZEOF_OFF_T=8 -I libmspack/mspack checks/cab_model_main.c libmspack/test/cab_model_test.c libmspack/mspack/system.c libmspack/mspack/cabd.c libmspack/mspack/lzxd.c libmspack/mspack/mszipd.c libmspack/mspack/qtmd.c -o "$build/model"
for group in model split partial; do
 "$build/model" "$group" | tee "$build/$group.log"
 grep -Eq "^CAB_MODEL_GROUP $group assertions=[1-9][0-9]*$" "$build/$group.log"
 grep -Fx CAB_MODEL_GROUP_COMPLETE "$build/$group.log"
done
bash checks/baseline.sh
node libmspack/test/cab_model_mutants.cjs | tee "$build/mutants.log"
grep -Fx 'MUTANT clear-history caught' "$build/mutants.log"
grep -Fx 'MUTANT pending-not-advanced caught' "$build/mutants.log"
echo CAB_MODEL_COMPLETE
