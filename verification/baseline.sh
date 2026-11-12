#!/usr/bin/env bash
set -euo pipefail
b=$(mktemp -d "${TMPDIR:-/tmp}/baseline.XXXXXX")
trap 'rm -rf "$b"' EXIT
cp src/eia608_from_utf8.c.cached "$b/eia608_from_utf8.c"
cc -std=c99 -I. -Icaption verification/baseline.c src/caption.c src/cea708.c src/eia608.c src/eia608_charmap.c src/utf8.c src/xds.c "$b/eia608_from_utf8.c" -o "$b/baseline"
"$b/baseline"
cc -std=c99 -Icaption unit_tests/test_wrap.c src/utf8.c -o "$b/wrap"
"$b/wrap"
echo BASELINE_WRAP_PASS
