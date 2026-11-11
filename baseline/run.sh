#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
build=$(mktemp -d)
trap 'rm -rf "$build"' EXIT
bash "$root/baseline/build.sh" "$build"
cc -std=c11 -I"$root/libyara/include" -I"$build" "$root/baseline/witness.c" "$build/.libs/libyara.a" -lpthread -lm -o "$build/witness"
"$build/witness"
