#!/usr/bin/env bash
set -euo pipefail
b=$(mktemp -d "${TMPDIR:-/tmp}/aubio.XXXXXX")
trap 'rm -rf "$b"' EXIT
mkdir "$b/pkg"
PKG_CONFIG_PATH= PKG_CONFIG_LIBDIR="$b/pkg" cmake -S . -B "$b/build" -DCMAKE_BUILD_TYPE=Release
cmake --build "$b/build" --target aubio -j 4
cc -std=c99 -Isrc baseline/smoke.c -L"$b/build/src" -Wl,-rpath,"$b/build/src" -laubio -lm -o "$b/smoke"
"$b/smoke"
cc -std=c99 -Isrc tests/src/pitch/test-pitch.c -L"$b/build/src" -Wl,-rpath,"$b/build/src" -laubio -lm -o "$b/old"
"$b/old"
echo BASELINE_OLD_PITCH_OK
