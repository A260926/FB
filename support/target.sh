#!/bin/bash
set -euo pipefail
b=$(mktemp -d)
trap 'rm -rf "$b"' EXIT
run_suite(){
 local dir=$1 out=$2
 (cd "$dir"; bash support/build.sh "$out"; cc -std=c99 -DHAVE_INTTYPES_H=1 -DHAVE_LRINTF=1 -Isupport -Iinclude -Ilibfaad support/driver.c support/tns_cases.c "$out/libfaad.a" -lm -o "$out/cases")
}
mkdir "$b/normal"
run_suite "$PWD" "$b/normal"
"$b/normal/cases" | tee "$b/output"
awk -v style=A -v groups="coefficients ranges stream" -f support/verify_output.awk "$b/output"
for mode in direction compression; do
 mkdir "$b/$mode" "$b/$mode-build"
 tar -cf - CMakeLists.txt properties.json include libfaad support | tar -xf - -C "$b/$mode"
 (cd "$b/$mode"; patch -s -p1 < "support/$mode.patch")
 run_suite "$b/$mode" "$b/$mode-build"
 set +e
 "$b/$mode-build/cases" >"$b/$mode.log" 2>&1
 status=$?
 set -e
 cat "$b/$mode.log"
 test "$status" = 42
 grep -q '^ASSERT ' "$b/$mode.log"
 echo "MUTANT_REJECTED $mode"
done
bash support/baseline.sh
