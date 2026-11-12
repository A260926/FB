#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-start}
case "$mode" in start|--legacy) ;; *) exit 2;; esac
build=$(mktemp -d baseline/.build.XXXXXX)
trap 'rm -rf "$build"' EXIT
export TMPDIR="$PWD/$build"
shopt -s nullglob
sources=(vm/ubpf_vm.c vm/ubpf_jit_x86_64.c vm/ubpf_local_*.c)
cc -std=gnu11 -O1 -include baseline/endian-compat.h -Ivm/inc -Ivm "${sources[@]}" baseline/legacy.c -lm -o "$build/legacy"
"$build/legacy"
if [ "$mode" = start ]; then
  cc -std=gnu11 -O1 -include baseline/endian-compat.h -Ivm/inc -Ivm "${sources[@]}" baseline/witness.c -lm -o "$build/witness"
  "$build/witness"
  echo UBPF_CALL_BASELINE_OK
fi
