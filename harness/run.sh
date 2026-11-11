#!/usr/bin/env bash
set -euo pipefail
test -s "${1:-harness/baseline.c}"
pkg-config --exact-version=1.6.1 opus
pkg-config --exact-version=1.3.6 ogg
build=$(mktemp -d ./.opus-build.XXXXXX)
trap 'rm -rf "$build"' EXIT
read -r -a flags <<< "$(pkg-config --cflags --libs opus ogg)"
cc -std=c99 -O1 harness/make_fixture.c "${flags[@]}" -lm -o "$build/fixture"
"$build/fixture" "$build/zero.opus" 0
"$build/fixture" "$build/six.opus" 1536
cc -std=c99 -O1 -DOP_HAVE_LRINTF -I include "${1:-harness/baseline.c}" src/info.c src/internal.c src/opusfile.c src/stream.c "${flags[@]}" -lm -o "$build/check"
"$build/check" "$build"
