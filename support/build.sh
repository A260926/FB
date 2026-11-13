#!/bin/bash
set -euo pipefail
for source in src/lib/*.c; do
 name=${source##*/}
 cc -std=c99 -O2 -Isrc/lib -c "$source" -o "$1/${name%.c}.o"
done
ar rcs "$1/libshine.a" "$1/"*.o
