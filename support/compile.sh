#!/bin/bash
set -euo pipefail
b=$1
root=$(pwd)
mkdir -p "$b/zobj"
(cd "$b/zobj"; cc -O1 -I"$root/support/zlib" -c "$root"/support/zlib/*.c; ar cr "$b/libz.a" ./*.o)
cmake -S . -B "$b/xlsx" -DBUILD_TESTS=OFF -DBUILD_EXAMPLES=OFF -DBUILD_SHARED_LIBS=OFF -DCMAKE_BUILD_TYPE=Release -DZLIB_LIBRARY="$b/libz.a" -DZLIB_INCLUDE_DIR="$root/support/zlib" -DCMAKE_ARCHIVE_OUTPUT_DIRECTORY="$b/lib"
cmake --build "$b/xlsx" --target xlsxwriter -j2
