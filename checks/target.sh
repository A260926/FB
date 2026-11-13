#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
test -s checks/target.c
test -s renderer/shaders/relief_shader.c
test -s renderer/shaders/relief_shader.h
d=$(mktemp -d)
trap 'rm -rf "$d"' EXIT
${CC:-cc} -std=c99 -O1 -Irenderer/core -Irenderer/shaders checks/target.c renderer/core/*.c renderer/shaders/relief_shader.c -lm -o "$d/target"
"$d/target" "$d"
sh checks/baseline.sh
