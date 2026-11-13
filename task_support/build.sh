#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p .task-build/objects
objects=()
for file in src/lib/*.c; do
 object=".task-build/objects/$(basename "${file%.c}").o"
 cc -std=c99 -O2 -Isrc/lib -c "$file" -o "$object"
 objects+=("$object")
done
ar rcs .task-build/libshine.a "${objects[@]}"
