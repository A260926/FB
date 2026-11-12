#!/usr/bin/env bash
set -euo pipefail
build=$(mktemp -d "${TMPDIR:-/tmp}/sbc-baseline.XXXXXX")
trap 'rm -rf "$build"' EXIT
make BUILD_DIR="$build/obj" BIN_DIR="$build/bin" libsbc -j4 >/dev/null
cc -std=c11 -UNDEBUG -Iinclude repro/baseline.c "$build/bin/libsbc.a" -lm -o "$build/baseline"
"$build/baseline"
