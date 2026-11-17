#!/bin/bash
set -euo pipefail
mkdir -p .task-build/production
rm -f .task-build/production/*.o
for source in math/*.c render/*.c; do
  object=.task-build/production/${source//\//_}.o
  cc -std=c99 -O2 -I. -Irender -c "$source" -o "$object"
done
cc -std=c99 -O2 -I. -Irender -Irender/tests -DPINGO_REFERENCE_DIR='"render/tests/references"' render/tests/test_render.c render/tests/memory_backend.c render/tests/golden.c .task-build/production/*.o -lm -o .task-build/baseline
