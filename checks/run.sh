#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
src=$1
shift
d=$(mktemp -d)
trap 'rm -rf "$d"' EXIT
cp -R src include "$d/"
sed 's/^#define TGL_FEATURE_ALT_RENDERMODES 0$/#define TGL_FEATURE_ALT_RENDERMODES 1/' "$d/include/zfeatures.h" > "$d/include/zfeatures.tmp"
mv "$d/include/zfeatures.tmp" "$d/include/zfeatures.h"
${CC:-cc} -std=c99 -O1 -Wno-shift-negative-value -Wno-unused-value -I"$d/include" -I"$d/src" "$src" "$d"/src/*.c -lm -o "$d/check"
"$d/check" "$@"
