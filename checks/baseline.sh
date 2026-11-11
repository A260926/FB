#!/usr/bin/env bash
set -euo pipefail
build=$(mktemp -d)
trap 'rm -rf "$build"' EXIT
cmake -S . -B "$build" -DWITH_OGG=OFF -DBUILD_PROGRAMS=OFF -DBUILD_EXAMPLES=OFF -DBUILD_DOCS=OFF -DBUILD_CXXLIBS=OFF -DBUILD_TESTING=OFF -DINSTALL_MANPAGES=OFF
cmake --build "$build" -j 4
cc -I include checks/baseline.c "$build/src/libFLAC/libFLAC.a" -lm -lpthread -o "$build/baseline"
(cd "$build" && ./baseline)
