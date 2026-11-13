#!/bin/bash
set -euo pipefail
b=$(mktemp -d)
trap 'rm -rf "$b"' EXIT
test -s test/joint_contract.c
awk -f scripts/mk_bits_lut > "$b/bits_lut.h"
cc -std=c99 -O1 -D_POSIX_C_SOURCE=199309L -D_C99_SOURCE -Iinc -Isrc -Ivendor -I"$b" src/*.c test/joint_contract.c task_support/driver.c -lm -o "$b/target"
"$b/target" > "$b/output"
cat "$b/output"
awk '$1=="GROUP"&&$2=="joint"&&$3~/^[1-9][0-9]*$/&&$4=="DONE"{n++}END{exit(n!=1)}' "$b/output"
awk '$1=="GROUP"&&$2=="ownership"&&$3~/^[1-9][0-9]*$/&&$4=="DONE"{n++}END{exit(n!=1)}' "$b/output"
awk '$1=="GROUP"&&$2=="compat"&&$3~/^[1-9][0-9]*$/&&$4=="DONE"{n++}END{exit(n!=1)}' "$b/output"
awk '$1=="ALL_GROUPS_DONE"&&$2~/^[1-9][0-9]*$/{n++}END{exit(n!=1)}' "$b/output"
bash task_support/baseline.sh
