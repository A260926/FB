#!/bin/bash
set -euo pipefail
mkdir -p .task-build
sources=(soxr data-io dbesi0 filter fft4g64 cr cr32 fft4g32 cr64 vr32)
objects=()
for unit in "${sources[@]}"; do
  cc -std=c99 -O2 -DSOXR_LIB -Isrc -Itask_support -I. -c "src/$unit.c" -o ".task-build/$unit.o"
  objects+=(".task-build/$unit.o")
done
ar rcs .task-build/libsoxr.a "${objects[@]}"
cc -std=c99 -O2 -Isrc task_support/baseline.c .task-build/libsoxr.a -lm -o .task-build/baseline
cc -std=c99 -O2 -Isrc tests/1-delay-clear.c .task-build/libsoxr.a -lm -o .task-build/old-delay
