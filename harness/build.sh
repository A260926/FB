#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p .build
for src in src/*.c; do
  cc -std=c99 -O1 -g -Isrc -c "$src" -o ".build/$(basename "${src%.c}").o"
done
ar rcs .build/libgumbo.a .build/*.o
