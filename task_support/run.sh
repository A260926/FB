#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT
sources=(api.c decode.c mem.c ops.c ops2.c prim_ops.c)
if [[ ${1:-baseline} == witness ]]; then
 cc -std=gnu11 -O1 -Iinclude "${sources[@]}" task_support/segment_witness.c -o "$out/witness"
 "$out/witness"
 exit
fi
if [[ ${1:-baseline} == target ]]; then
 test -s test/segment_fault_cases.c || { echo 'MISSING test/segment_fault_cases.c' >&2; exit 2; }
 cc -std=gnu11 -O1 -Iinclude -I. "${sources[@]}" test/segment_fault_cases.c -o "$out/segments"
 "$out/segments" | tee "$out/target.log"
 for group in ranges permissions restart; do grep -qx "PASS $group" "$out/target.log"; done
 grep -qx 'X86EMU_SEGMENT_FAULT_OK' "$out/target.log"
fi
cc -std=gnu11 -O1 -Iinclude "${sources[@]}" task_support/baseline.c -o "$out/baseline"
"$out/baseline"
