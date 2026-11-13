#!/bin/bash
set -euo pipefail
b=$(mktemp -d)
trap 'rm -rf "$b"' EXIT
bash support/build.sh "$b"
cc -std=c99 -Iinclude -I"$b/include" support/baseline.c "$b/libsndfile.a" -lm -o "$b/baseline"
(cd "$b"; ./baseline)
