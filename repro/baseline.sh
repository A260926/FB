#!/usr/bin/env bash
set -euo pipefail
src=$(pwd)
build=$(mktemp -d "${TMPDIR:-/tmp}/theora-baseline.XXXXXX")
trap 'rm -rf "$build"' EXIT
(cd "$build" && "$src/configure" --disable-shared --enable-static --disable-examples --disable-doc --disable-spec --disable-asm >/dev/null)
make -C "$build" -j4 >/dev/null
make -C "$build/tests" granulepos comment >/dev/null
"$build/tests/granulepos"
"$build/tests/comment"
cc -std=c99 -UNDEBUG -DHAVE_CONFIG_H -I"$build" -Iinclude -Ilib $(pkg-config --cflags ogg) repro/filter_smoke.c "$build/lib/.libs/libtheoradec.a" $(pkg-config --libs ogg) -lm -o "$build/filter_smoke"
"$build/filter_smoke"
printf 'THEORA_BASELINE_OK granulepos=6 comment=1\n'
