#!/usr/bin/env bash
set -euo pipefail

#
# compiles the real renderer library sources together with check-pcss.c in a
# temporary directory, runs the pcss sanity program and only prints
# PCSS_SHADOW_OK when every numerical case passes. the optional first argument
# selects the directory for the ppm comparison image.
#

outdir=${1:-"$PWD/sanity"}
mkdir -p "$outdir"

build=$(mktemp -d "${TMPDIR:-/tmp}/check-pcss.XXXXXX")
trap 'rm -r "$build"' EXIT

cc -std=c99 -O1 -Wall -Wextra -pedantic \
    -Irenderer/core \
    sanity/check-pcss.c \
    renderer/core/*.c \
    renderer/shaders/*.c \
    -lm -o "$build/check-pcss"

"$build/check-pcss" "$outdir"
