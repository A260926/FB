#!/bin/bash
set -euo pipefail
test -s checks/gray_contract.c
build=$(mktemp -d)
trap 'rm -rf "$build"' EXIT
bash checks/build-lib.sh "$build"
cc -std=c11 -O1 -Isrc -I"$build/src" -Ichecks checks/contract_main.c checks/gray_contract.c "$build/src/libleptonica.a" -lm -o "$build/target"
"$build/target" thresholds storage derived
for old in baseline old_numeric; do
 cc -std=c11 -O1 -Isrc -I"$build/src" "checks/$old.c" "$build/src/libleptonica.a" -lm -o "$build/$old"
 "$build/$old"
done
for name in saturation left-edge; do
 mkdir -p "$build/$name/src"
 cp src/seedfill.c "$build/$name/src/seedfill.c"
 patch -s -p1 -d "$build/$name" < "checks/mutants/$name.patch"
 cc -std=c11 -O1 -Isrc -I"$build/src" -c "$build/$name/src/seedfill.c" -o "$build/$name/seedfill.o"
 cc -std=c11 -O1 -Isrc -I"$build/src" -Ichecks checks/contract_main.c checks/gray_contract.c "$build/$name/seedfill.o" "$build/src/libleptonica.a" -lm -o "$build/$name/target"
 set +e
 "$build/$name/target" thresholds storage derived > "$build/$name/result.log" 2>&1
 status=$?
 set -e
 cat "$build/$name/result.log"
 if [ "$status" -ne 5 ] || ! grep -q GRAY_MISMATCH "$build/$name/result.log"; then
  echo "MUTANT_NOT_CONTENT_REJECTED $name status=$status" >&2; exit 1
 fi
 echo "GRAY_MUTANT_REJECTED $name"
done
echo GRAY_MODEL_AND_MUTANTS_OK
