#!/usr/bin/env bash
# Regression test for chained native FLAC decoding (no Ogg involved).
# Builds a static libFLAC with WITH_OGG=OFF in a temporary directory,
# compiles the native_chain driver against it and runs all scenarios.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
build=$(mktemp -d)
trap 'rm -rf "$build"' EXIT
cmake -S "$root" -B "$build" -DWITH_OGG=OFF -DBUILD_PROGRAMS=OFF -DBUILD_EXAMPLES=OFF -DBUILD_DOCS=OFF -DBUILD_CXXLIBS=OFF -DBUILD_TESTING=OFF -DINSTALL_MANPAGES=OFF
cmake --build "$build" -j 4
cc -I "$root/include" "$here/native_chain.c" "$build/src/libFLAC/libFLAC.a" -lm -lpthread -o "$build/native_chain"
"$build/native_chain" > "$build/native_chain.log"
# only print NATIVE_CHAIN_OK when the driver actually ran all checks
grep -q NATIVE_CHAIN_DRIVER_OK "$build/native_chain.log"
cat "$build/native_chain.log"
echo NATIVE_CHAIN_OK
