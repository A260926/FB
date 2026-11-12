#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
build=$(mktemp -d)
trap 'rm -rf "$build"' EXIT
cc -std=c11 -O1 -I. tlsf.c baseline/smoke.c -o "$build/smoke"
"$build/smoke"
