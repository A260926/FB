#!/usr/bin/env bash
#
# tests/quality_map/run.sh
#
# Build this library (both a threaded and a non-threaded variant) and run the
# quality map regression driver against both. The test driver itself encodes its
# sample files with the library's own encoder, so nothing external is required.

set -euo pipefail

here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
work=$(mktemp -d "${TMPDIR:-/tmp}/quality_map.XXXXXX")
trap 'rm -rf "$work"' EXIT

build_lib () {
    local dir=$1 threads=$2
    cmake -S "$root" -B "$dir" \
        -DWAVPACK_BUILD_PROGRAMS=OFF -DBUILD_TESTING=OFF \
        -DBUILD_SHARED_LIBS=OFF -DWAVPACK_ENABLE_THREADS="$threads" \
        > "$dir.cmake.log"
    cmake --build "$dir" -j 4 > "$dir.build.log"
}

run_variant () {
    local dir=$1 label=$2
    cc -O2 -Wall -Wextra -I "$root/include" -I "$root/tests/quality_map" \
        "$root/tests/quality_map/quality_map.c" "$dir/libwavpack.a" \
        -lm $([[ $label == threaded ]] && echo -pthread) -o "$dir/quality_map"
    echo "----- $label -----"
    (cd "$work" && "$dir/quality_map")
}

build_lib "$work/build-threaded" ON
build_lib "$work/build-serial" OFF

run_variant "$work/build-threaded" threaded
run_variant "$work/build-serial" serial

echo "QUALITY_MAP_ALL_OK"
