#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
b=$(mktemp -d)
trap 'rm -rf "$b"' EXIT
cp src/eia608_from_utf8.c.cached "$b/eia608_from_utf8.c"
src=(src/caption.c src/cea708.c src/eia608.c src/eia608_charmap.c "$b/eia608_from_utf8.c" src/mpeg.c src/scc.c src/srt.c src/utf8.c src/vtt.c src/xds.c)
cc -std=c99 -O1 -I. -Icaption "${src[@]}" task_support/baseline.c -lm -o "$b/probe"
"$b/probe"
cc -std=c99 -O1 -I. -Icaption "${src[@]}" unit_tests/test_wrap.c -o "$b/wrap"
"$b/wrap"
echo UPSTREAM_WRAP_PASS
