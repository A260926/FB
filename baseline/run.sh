#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
root=$PWD
work=$(mktemp -d "${TMPDIR:-/tmp}/onig-compile-baseline.XXXXXX")
trap 'rm -rf "$work"' EXIT
for mode in direct switch; do
    flags=''
    if [ "$mode" = switch ]; then flags='-DONIG_DISABLE_DIRECT_THREADING'; fi
    cmake -S "$root" -B "$work/$mode" -DBUILD_SHARED_LIBS=OFF \
        -DCMAKE_BUILD_TYPE=Release -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DCMAKE_C_FLAGS="$flags"
    cmake --build "$work/$mode" --parallel 2
    "${CC:-cc}" -std=c99 -O2 -DHAVE_CONFIG_H -DONIG_STATIC $flags \
        -I "$work/$mode" -I "$root/src" "$root/baseline/probe.c" \
        "$work/$mode/libonig.a" -o "$work/probe-$mode"
    "$work/probe-$mode"
done
