#!/usr/bin/env bash
set -euo pipefail
test -s tests/coefficients.c
test -s tests/run_coefficients.mjs
b=$(mktemp -d)
trap 'rm -rf "$b"' EXIT
node tests/run_coefficients.mjs | tee "$b/out"
grep -Eq '^MPEG_COEFF_OK cases=[1-9][0-9]*$' "$b/out"
for marker in MATRIX_MAP_CAUGHT ODDIFY_CAUGHT WORKSPACE_CLEAR_CAUGHT; do grep -Fxq "$marker" "$b/out"; done
bash baseline/run.sh
