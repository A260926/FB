#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
b=$(mktemp -d .refresh-old.XXXXXX)
trap 'rm -rf "$b"' EXIT
cc -std=gnu11 -O3 -ffast-math -DNDEBUG -Wno-pointer-to-int-cast -DH264E_MAX_THREADS=0 -include refresh_support/platform.h -I. minih264e_test.c -lm -o "$b/encoder"
"$b/encoder" vectors/foreman.cif "$b/out.264"
cmp "$b/out.264" vectors/out_ref.264
bytes=$(wc -c < "$b/out.264" | tr -d ' ')
printf 'UPSTREAM_FOREMAN bytes=%s reference=exact\n' "$bytes"
