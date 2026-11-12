#!/bin/sh
set -eu
test -s tests/demux_chunks.c || { echo 'missing target: tests/demux_chunks.c' >&2; exit 1; }
build=$(mktemp -d)
trap 'rm -r "$build"' EXIT
cc -std=c99 -O2 tests/demux_chunks.c -lm -o "$build/target"
"$build/target" > "$build/result" 2>&1 || { cat "$build/result"; exit 1; }
cat "$build/result"
for marker in PS_CHUNKS_OK PS_HEADERS_OK PS_BOUNDED_OK PS_REWIND_OK; do grep -Fx "$marker" "$build/result" >/dev/null; done
sh harness/baseline.sh
