#!/usr/bin/env bash
set -euo pipefail
build=$(mktemp -d "${TMPDIR:-/tmp}/current.XXXXXX")
trap 'rm -r "$build"' EXIT
cc -std=c99 -O1 -Irenderer/core sanity/current.c renderer/core/*.c renderer/shaders/*.c -lm -o "$build/current"
"$build/current"
