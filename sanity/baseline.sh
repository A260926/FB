#!/usr/bin/env bash
set -euo pipefail
build=$(mktemp -d)
trap 'rm -rf "$build"' EXIT
cmake -S . -B "$build" -DWAVPACK_BUILD_PROGRAMS=OFF -DBUILD_TESTING=OFF -DBUILD_SHARED_LIBS=OFF
cmake --build "$build" -j 4
cc -I include sanity/baseline.c "$build/libwavpack.a" -lm -lpthread -o "$build/baseline"
(cd "$build" && ./baseline)
