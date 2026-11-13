#!/bin/bash
set -euo pipefail
b=$(mktemp -d)
trap 'rm -rf "$b"' EXIT
TRACE=1 bash task_support/build.sh "$b"
cc -std=c99 -O2 -DVORBIS_COUPLING_TRACE -Iinclude -Ilib -Itask_support -Itask_support/vendor/ogg/include task_support/driver.c task_support/target_cases.c "$b/libcodec.a" -lm -o "$b/target"
"$b/target" > "$b/output" 2>&1 || { rc=$?; cat "$b/output"; exit "$rc"; }
cat "$b/output"
grep -Eq "^GROUP quadrants [1-9][0-9]* COMPLETE$" "$b/output"
grep -Eq "^GROUP chain [1-9][0-9]* COMPLETE$" "$b/output"
grep -Eq "^GROUP packets [1-9][0-9]* COMPLETE$" "$b/output"
grep -qx "VORBIS_COUPLING_OK" "$b/output"
for mutant in sign order; do
 mkdir "$b/$mutant"
 cp lib/mapping0.c "$b/$mutant/mapping0.c"
 patch -s "$b/$mutant/mapping0.c" "task_support/mutations/$mutant.patch"
 TRACE=1 MAPPING_SOURCE="$b/$mutant/mapping0.c" bash task_support/build.sh "$b/$mutant"
 cc -std=c99 -O2 -DVORBIS_COUPLING_TRACE -Iinclude -Ilib -Itask_support -Itask_support/vendor/ogg/include task_support/driver.c task_support/target_cases.c "$b/$mutant/libcodec.a" -lm -o "$b/$mutant/test"
 set +e
 TASK_MUTANT="$mutant" "$b/$mutant/test" > "$b/$mutant/out" 2>&1
 rc=$?
 set -e
 cat "$b/$mutant/out"
 test "$rc" = 42
 grep -q '^ASSERT ' "$b/$mutant/out"
 echo "MUTANT_CAUGHT $mutant"
done
bash task_support/baseline.sh
