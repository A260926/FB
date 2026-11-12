#!/usr/bin/env bash
set -euo pipefail
export PATH="$PWD/.build-tools/bin:$PATH"
b=$(mktemp -d)
trap 'rm -rf "$b"' EXIT
.build-tools/bin/meson setup "$b" --buildtype=release --default-library=static -Denable_asm=false -Denable_tools=false -Denable_tests=false -Denable_examples=false -Dtrim_dsp=false >"$b/config.log" 2>&1 || { cat "$b/config.log"; exit 1; }
.build-tools/bin/ninja -C "$b" -j4 >"$b/build.log" 2>&1 || { cat "$b/build.log"; exit 1; }
cc -std=c11 -DBITDEPTH=8 -I"$b" -I"$b/include" -I. -Iinclude verification/baseline.c "$b/src/libdav1d.a" -lpthread -lm -o "$b/baseline"
"$b/baseline"
cc -std=c11 -I"$b/include" -Iinclude verification/decode_fixture.c "$b/src/libdav1d.a" -lpthread -lm -o "$b/decode"
"$b/decode" verification/fixtures/420-8.ivf
"$b/decode" verification/fixtures/420-10.ivf
