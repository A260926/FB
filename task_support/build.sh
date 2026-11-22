#!/bin/bash
set -euo pipefail
mkdir -p .task-build
objects=()
for source in libspeex/*.c; do
  case "$source" in libspeex/testenc*|libspeex/fftwrap.c|libspeex/kiss_fft.c|libspeex/kiss_fftr.c|libspeex/smallft.c|libspeex/vorbis_psy.c) continue;; esac
  object=".task-build/$(basename "$source" .c).o"
  cc -std=c99 -O1 -DEXPORT= -DFLOATING_POINT -DUSE_ALLOCA -DHAVE_ALLOCA_H -DSPEEX_TEST_COUNTERS -Iinclude -Itask_support -I. -c "$source" -o "$object"
  objects+=("$object")
done
rm -f .task-build/libspeex.a
ar rcs .task-build/libspeex.a "${objects[@]}"
cc -std=c99 -O1 -Iinclude -Itask_support -I. task_support/baseline.c .task-build/libspeex.a -lm -o .task-build/baseline
