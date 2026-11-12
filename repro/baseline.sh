#!/usr/bin/env bash
set -euo pipefail
build=$(mktemp -d "${TMPDIR:-/tmp}/soundpipe-baseline.XXXXXX")
trap 'rm -rf "$build"' EXIT
make CONFIG=repro/minimal.mk INTERMEDIATES_PREFIX="$build" "$build/libsoundpipe.a" -j4 >/dev/null
cc -std=c99 -UNDEBUG -I"$build/h" repro/baseline.c "$build/libsoundpipe.a" -lm -o "$build/baseline"
"$build/baseline"
