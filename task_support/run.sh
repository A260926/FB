#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT
if [[ ${1:-baseline} == target ]]; then
 test -s tests/z80_fetch_trace_cases.c || { echo 'MISSING tests/z80_fetch_trace_cases.c' >&2; exit 2; }
 cc -std=c11 -O1 -I. tests/z80_fetch_trace_cases.c -o "$out/trace"
 "$out/trace" | tee "$out/target.log"
 for group in bytes waits interrupts; do grep -qx "PASS $group" "$out/target.log"; done
 grep -qx 'Z80_FETCH_TRACE_OK' "$out/target.log"
fi
cc -std=c11 -O1 -I. task_support/baseline.c -o "$out/baseline"
"$out/baseline"
cc -std=c11 -O1 -I. task_support/upstream_tests/z80-test.c -o "$out/legacy"
"$out/legacy" > "$out/legacy.log"
tail -5 "$out/legacy.log"
