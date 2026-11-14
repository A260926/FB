#!/bin/bash
set -euo pipefail
mkdir -p .task-build
# Compile palette_search verbatim from the current production file.
{
 echo '#include "task_support/headless.h"'
 sed -n '/^int palette_search(/,/^}/p' src/palette.c
} > .task-build/palette_core.c
test "$(grep -c '^int palette_search(' .task-build/palette_core.c)" = 1
task_objects=''
for file in src/volume.c src/marchingcube.c src/volume_to_vertices.c src/utils/color.c .task-build/palette_core.c src/volume_samples.c; do
 [[ -f "$file" ]] || continue
 obj=".task-build/$(basename "$file" .c).o"
 task_header="-include task_support/headless.h"
 [[ "$file" != src/volume.c ]] || task_header=""
 cc -std=gnu11 -O1 -I. -Isrc -Iext_src/uthash $task_header -c "$file" -o "$obj"
 task_objects="$task_objects $obj"
done
c++ -std=c++14 -O1 -c src/meshoptimizer.cpp -o .task-build/meshoptimizer.o
rm -f .task-build/production.a
ar rcs .task-build/production.a $task_objects .task-build/meshoptimizer.o
cc -std=gnu11 -O1 -I. -Isrc -Itask_support -c task_support/baseline.c -o .task-build/baseline.o
c++ .task-build/baseline.o .task-build/production.a -lm -o .task-build/baseline
