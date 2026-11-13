#!/bin/bash
set -euo pipefail
b=$(mktemp -d)
trap 'rm -rf "$b"' EXIT
bash task_support/build.sh "$b"
cc -std=c99 -O2 -Iinclude -Itask_support/vendor/ogg/include task_support/baseline.c "$b/libcodec.a" -lm -o "$b/base"
"$b/base"
cc -std=c99 -O2 -Iinclude -Itask_support/vendor/ogg/include test/test_codebook.c "$b/libcodec.a" -lm -o "$b/codebook"
"$b/codebook"
echo VORBIS_OLD_CODEBOOK_OK
