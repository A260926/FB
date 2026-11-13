#!/bin/bash
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
mode=${1:-baseline}
if [[ "$mode" == target ]]; then test -s "$root/test/default/hpke_contract.c"; fi
owned=0
if [[ -n "${CHECK_BUILD:-}" ]]; then build=$CHECK_BUILD; else build=$(mktemp -d); owned=1; fi
trap 'if [[ "$owned" == 1 ]]; then rm -rf "$build"; fi' EXIT
mkdir -p "$build"
if [[ ! -f "$build/Makefile" ]]; then (cd "$build"; "$root/configure" --disable-shared --disable-asm --disable-dependency-tracking) > "$build/configure.log" 2>&1; fi
make -s -j4 -C "$build/src/libsodium" > "$build/make.log" 2>&1
inc=( -I"$build/src/libsodium/include" -I"$root/src/libsodium/include" -I"$root/src/libsodium/include/sodium" -I"$root/support" )
lib="$build/src/libsodium/.libs/libsodium.a"
if [[ "$mode" == baseline ]]; then
 cc -std=c99 "${inc[@]}" "$root/support/baseline.c" "$lib" -lpthread -o "$build/witness"
 "$build/witness"
else
 cc -std=c99 "${inc[@]}" "$root/support/harness.c" "$root/test/default/hpke_contract.c" "$lib" -lpthread -o "$build/contract"
 "$build/contract" | tee "$build/contract.log"
 for group in vectors binding contexts; do grep -Eq "^GROUP $group checks=[1-9][0-9]*$" "$build/contract.log"; done
 grep -qx 'CONTRACT_COMPLETE 3' "$build/contract.log"
fi
make -s -j4 -C "$build/test/default" scalarmult kdf_hkdf aead_chacha20poly1305 > "$build/legacy-build.log" 2>&1
for old in scalarmult kdf_hkdf aead_chacha20poly1305; do (cd "$build/test/default"; "./$old"); echo "LEGACY_${old}_OK"; done
echo "RUN_${mode}_COMPLETE"
