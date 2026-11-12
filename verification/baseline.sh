#!/usr/bin/env bash
set -euo pipefail
b=$(mktemp -d)
trap 'rm -rf "$b"' EXIT
cmake -S . -B "$b" -DCMAKE_BUILD_TYPE=Release -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DBUILD_TESTING=ON -DOPUS_ENABLE_FLOAT_API=ON -DBUILD_SHARED_LIBS=OFF >"$b/config.log" 2>&1 || { cat "$b/config.log"; exit 1; }
cmake --build "$b" --target opus test_opus_padding -j 4 >"$b/build.log" 2>&1 || { cat "$b/build.log"; exit 1; }
cc -std=c99 -Iinclude verification/baseline.c "$b/libopus.a" -lm -o "$b/baseline"
"$b/baseline"
"$b/test_opus_padding"
echo BASELINE_OPUS_PADDING_PASS
