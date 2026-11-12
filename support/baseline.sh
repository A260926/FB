#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
work=$(mktemp -d "${TMPDIR:-/tmp}/nsvg-baseline.XXXXXX")
trap 'rm -rf "$work"' EXIT
cc -std=c99 -O2 -Isrc support/nanosvg_impl.c support/baseline.c -lm -o "$work/baseline"
"$work/baseline" "$@"
