#!/bin/bash
set -euo pipefail
b=$(mktemp -d)
trap 'rm -rf "$b"' EXIT
test -s test_src/checkpoint_fuzz.c
cc -std=c99 -O1 -Isljit_src test_src/checkpoint_fuzz.c task_support/driver.c sljit_src/sljitLir.c -o "$b/target"
"$b/target" > "$b/output"
cat "$b/output"
awk '$1=="GROUP"&&$2=="model"&&$3~/^[1-9][0-9]*$/&&$4=="DONE"{n++}END{exit(n!=1)}' "$b/output"
awk '$1=="GROUP"&&$2=="resume"&&$3~/^[1-9][0-9]*$/&&$4=="DONE"{n++}END{exit(n!=1)}' "$b/output"
awk '$1=="GROUP"&&$2=="lifetime"&&$3~/^[1-9][0-9]*$/&&$4=="DONE"{n++}END{exit(n!=1)}' "$b/output"
awk '$1=="ALL_GROUPS_DONE"&&$2~/^[1-9][0-9]*$/{n++}END{exit(n!=1)}' "$b/output"
bash task_support/baseline.sh
case "$(uname -m)" in arm64|aarch64) ;; *) echo "ARM64 required" >&2; exit 2;; esac
mkdir -p "$b/mutant"
cp -R sljit_src "$b/mutant/"
patch -s -d "$b/mutant" -p1 < task_support/flags.patch
cc -std=c99 -O1 -I"$b/mutant/sljit_src" test_src/checkpoint_fuzz.c task_support/driver.c "$b/mutant/sljit_src/sljitLir.c" -o "$b/mutant-test"
set +e
"$b/mutant-test" > "$b/mutant.log" 2>&1
rc=$?
set -e
cat "$b/mutant.log"
test "$rc" = 41
grep -qx 'CONTENT_DIFFERENCE resume' "$b/mutant.log"
cc -std=c99 -O1 -DSLJIT_HAVE_CONFIG_PRE=1 -Itest_src -I"$b/mutant/sljit_src" test_src/sljitMain.c test_src/sljitTest.c "$b/mutant/sljit_src/sljitLir.c" -lpthread -o "$b/mutant-old"
"$b/mutant-old" -s
echo SLJIT_CHECKPOINT_FUZZ_OK
