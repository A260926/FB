#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p .output
cc -std=c99 -O2 -Isrc example/example2.c -lm -o .output/upstream-example
rm -f .output/svg.png
(cd .output && ./upstream-example)
test -s .output/svg.png
signature=$(od -An -tx1 -N8 .output/svg.png | tr -d ' \n')
test "$signature" = 89504e470d0a1a0a
printf 'UPSTREAM_EXAMPLE_OK png_signature=%s\n' "$signature"
