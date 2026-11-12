#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
build=$(mktemp -d)
trap 'rm -rf "$build"' EXIT
cmake -S . -B "$build/build" -DBUILD_DWARFDUMP=OFF -DBUILD_DWARFGEN=OFF -DBUILD_DWARFEXAMPLE=OFF -DENABLE_DECOMPRESSION=OFF -DDO_TESTING=ON 2>&1 | tee "$build/configure.log"
cmake --build "$build/build" --target dwarf-static selfleb -j 4 2>&1 | tee "$build/build.log"
c++ -std=c++17 -DLIBDWARF_STATIC -Isrc/lib/libdwarf baseline/witness.cpp "$build/build/src/lib/libdwarf/libdwarf.a" -o "$build/witness"
"$build/witness" "$build" "$@"
"$build/build/test/selfleb"
echo LIBDWARF_OLD_LEB_TEST_OK
