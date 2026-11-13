#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
d=$(mktemp -d)
trap 'rm -rf "$d"' EXIT
${CC:-cc} -std=c99 -O1 -Irenderer/core checks/baseline.c renderer/core/graphics.c renderer/core/maths.c -lm -o "$d/base"
"$d/base"
