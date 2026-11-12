#!/usr/bin/env bash
set -euo pipefail
build=$(mktemp -d "${TMPDIR:-/tmp}/openaptx-baseline.XXXXXX")
trap 'rm -rf "$build"' EXIT
cc -std=c99 -O2 -UNDEBUG -DAPTX_FORCE_SCALAR -I. openaptx.c repro/baseline.c -o "$build/baseline"
"$build/baseline"
