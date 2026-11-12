#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
build=$(mktemp -d baseline/.target.XXXXXX)
trap 'rm -rf "$build"' EXIT
export TMPDIR="$PWD/$build"
# The fixed driver links the exact new C path. Missing/empty/undefined groups fail at compile/link.
shopt -s nullglob
sources=(vm/ubpf_vm.c vm/ubpf_jit_x86_64.c vm/ubpf_local_*.c)
cc -std=gnu11 -O1 -include baseline/endian-compat.h -Ivm/inc -Ivm "${sources[@]}" tests/local_calls.c baseline/target-main.c -lm -o "$build/target"
"$build/target" >"$build/stdout"
cat "$build/stdout"
awk '
  /^PASS (call_graph|frames|stack_bounds|helpers_jit) assertions=[1-9][0-9]*$/ { seen[$2]++; count++; next }
  /^UBPF_LOCAL_CALLS_EXECUTED groups=4 assertions=[1-9][0-9]*$/ { done++ }
  END { if(count!=4 || seen["call_graph"]!=1 || seen["frames"]!=1 || seen["stack_bounds"]!=1 || seen["helpers_jit"]!=1 || done!=1) exit 3 }
' "$build/stdout"
bash baseline/run.sh --legacy
echo UBPF_LOCAL_CALLS_TARGET_OK
