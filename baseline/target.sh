#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root"
test -s tests/pinvoke_contract.c || { echo 'Missing tests/pinvoke_contract.c' >&2; exit 1; }
build=$(mktemp -d)
trap 'rm -rf "$build"' EXIT
bash baseline/build.sh "$build"
cc -std=c11 -Ilibyara/include -I"$build" tests/pinvoke_contract.c "$build/.libs/libyara.a" -lpthread -lm -o "$build/contract"
"$build/contract" | tee "$build/result"
for case in basic member_identity wide_heaps bad_rows duplicate_imports empty repeat_scan; do
  grep -Fx "PASS $case" "$build/result" >/dev/null
done
printf 'PINVOKE_CONTRACT_OK\n'
