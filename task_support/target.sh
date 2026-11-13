#!/bin/bash
set -euo pipefail
b=$(mktemp -d)
trap 'rm -rf "$b"' EXIT
test -s tests/completion_fuzz.c
cc -std=c99 -O1 -I. one.c tests/completion_fuzz.c task_support/driver.c -lm -o "$b/target"
"$b/target" > "$b/output"
cat "$b/output"
awk '$1=="GROUP"&&$2=="finally"&&$3~/^[1-9][0-9]*$/&&$4=="DONE"{n++}END{exit(n!=1)}' "$b/output"
awk '$1=="GROUP"&&$2=="catch"&&$3~/^[1-9][0-9]*$/&&$4=="DONE"{n++}END{exit(n!=1)}' "$b/output"
awk '$1=="GROUP"&&$2=="abrupt"&&$3~/^[1-9][0-9]*$/&&$4=="DONE"{n++}END{exit(n!=1)}' "$b/output"
awk '$1=="ALL_GROUPS_DONE"&&$2~/^[1-9][0-9]*$/{n++}END{exit(n!=1)}' "$b/output"
bash task_support/baseline.sh
for kind in finally catch; do
 mkdir -p "$b/$kind"
 cp ./*.c ./*.h "$b/$kind/"
 patch -s -d "$b/$kind" -p1 < "task_support/mutations/$kind.patch"
 cc -std=c99 -O1 -I"$b/$kind" "$b/$kind/one.c" tests/completion_fuzz.c task_support/driver.c -lm -o "$b/mutant"
 set +e
 "$b/mutant" > "$b/mutant.log" 2>&1
 rc=$?
 set -e
 cat "$b/mutant.log"
 test "$rc" = 41
 grep -qx "CONTENT_DIFFERENCE $kind" "$b/mutant.log"
done
echo MUJS_MODELFUZZ_OK
