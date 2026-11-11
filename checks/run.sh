#!/usr/bin/env bash
set -euo pipefail
file=$1
test -s "$file"
b=$(mktemp -d "${TMPDIR:-/tmp}/speex-check.XXXXXX")
trap 'rm -rf "$b"' EXIT
cc -std=c99 -O1 -DFLOATING_POINT -DUSE_SMALLFT -DEXPORT= -Iinclude "$file" \
 libspeexdsp/mdf.c libspeexdsp/preprocess.c libspeexdsp/filterbank.c libspeexdsp/fftwrap.c libspeexdsp/smallft.c -lm -o "$b/check"
"$b/check"
