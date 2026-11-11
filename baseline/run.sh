#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
build=$(mktemp -d)
trap 'rm -rf "$build"' EXIT
bash "$root/baseline/build.sh" "$build"
cc -std=c11 -I"$root/include" "$root/baseline/witness.c" "$build/libcapstone.a" -o "$build/witness"
"$build/witness"
