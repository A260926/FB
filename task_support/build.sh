#!/bin/bash
set -euo pipefail
mkdir -p .task-build
for name in decode syn syn-intel syn-att udis86; do
 cc -std=c99 -O1 -DHAVE_STRING_H=1 -I. -Ilibudis86 -Itask_support/generated -Itask_support/generated/libudis86 -c "libudis86/$name.c" -o ".task-build/$name.o"
done
cc -std=c99 -O1 -DHAVE_STRING_H=1 -I. -Ilibudis86 -Itask_support/generated -Itask_support/generated/libudis86 -c task_support/generated/libudis86/itab.c -o .task-build/itab.o
ar rcs .task-build/libudis.a .task-build/decode.o .task-build/syn.o .task-build/syn-intel.o .task-build/syn-att.o .task-build/udis86.o .task-build/itab.o
cc -std=c99 -O1 -DHAVE_STRING_H=1 -I. -Itask_support/generated -Itask_support/generated/libudis86 tests/libcheck.c .task-build/libudis.a -o .task-build/libcheck
cc -std=c99 -O1 -DHAVE_STRING_H=1 -I. -Itask_support/generated -Itask_support/generated/libudis86 task_support/baseline.c .task-build/libudis.a -o .task-build/baseline
