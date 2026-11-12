#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
build=$(mktemp -d)
trap 'rm -rf "$build"' EXIT
cc -std=c11 -O1 -I. tlsf.c tests/allocator_model.c baseline/target-main.c -o "$build/target"
"$build/target"
sed 's/memcpy(p, ptr, minsize);/memcpy(p, ptr, minsize \/ 2);/' tlsf.c > "$build/copy.c"
sed 's/void\* aligned = align_ptr(ptr, align);/void* aligned = align_ptr(ptr, ALIGN_SIZE);/' tlsf.c > "$build/align.c"
for mutation in copy align; do
  cc -std=c11 -O1 -I. "$build/$mutation.c" tests/allocator_model.c baseline/target-main.c -o "$build/$mutation"
  status=0
  "$build/$mutation" > "$build/$mutation.log" 2>&1 || status=$?
  cat "$build/$mutation.log"
  test "$status" -eq 42
  grep -q '^MISMATCH ' "$build/$mutation.log"
  echo "CAPTURED $mutation semantic-mismatch"
done
bash baseline/run.sh
