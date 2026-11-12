#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
build=$(mktemp -d)
trap 'rm -rf "$build"' EXIT
cc -std=c99 -O2 -DED25519_NO_SEED -Isrc src/*.c baseline/smoke.c -o "$build/smoke"
"$build/smoke"
