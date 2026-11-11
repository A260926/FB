#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root"
test -s tests/arm64_address_contract.c
test -s tests/run_arm64_mutations.sh
build=$(mktemp -d)
trap 'rm -rf "$build"' EXIT
bash baseline/build.sh "$build"
cc -std=c11 -Iinclude tests/arm64_address_contract.c "$build/libcapstone.a" -o "$build/contract"
"$build/contract" | tee "$build/result"
for name in unsigned_offset signed_pre post_index register_offset pair sp_base api_reuse; do grep -Fx "PASS $name" "$build/result" >/dev/null; done
bash tests/run_arm64_mutations.sh | tee "$build/mutations"
for name in post_index signed_offset base_writeback; do grep -Fx "KILLED $name" "$build/mutations" >/dev/null; done
printf 'ARM64_ADDRESS_CONTRACT_OK\n'
