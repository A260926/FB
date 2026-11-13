#!/bin/bash
set -euo pipefail
b=$(mktemp -d)
trap 'rm -rf "$b"' EXIT
test -s test/relocated_loader_contract.cc
c++ -std=c++11 -O0 -Ielf -Idwarf -Itask_support elf/*.cc dwarf/*.cc test/relocated_loader_contract.cc -x c++ task_support/driver.c -o "$b/target"
"$b/target" > "$b/output"
cat "$b/output"
awk '$1=="GROUP"&&$2=="relocations"&&$3~/^[1-9][0-9]*$/&&$4=="DONE"{n++}END{exit(n!=1)}' "$b/output"
awk '$1=="GROUP"&&$2=="dwarf"&&$3~/^[1-9][0-9]*$/&&$4=="DONE"{n++}END{exit(n!=1)}' "$b/output"
awk '$1=="GROUP"&&$2=="ownership"&&$3~/^[1-9][0-9]*$/&&$4=="DONE"{n++}END{exit(n!=1)}' "$b/output"
awk '$1=="ALL_GROUPS_DONE"&&$2~/^[1-9][0-9]*$/{n++}END{exit(n!=1)}' "$b/output"
bash task_support/baseline.sh
