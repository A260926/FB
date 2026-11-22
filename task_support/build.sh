#!/bin/bash
set -euo pipefail
mkdir -p .task-build
flag=OFF
if [[ ${1:-baseline} == target ]]; then
  test -s src/SDL_sound_loop.c
  test -s src/SDL_sound_loop.h
  test -s examples/loop_region.c
  flag=ON
fi
if ! cmake -S task_support -B .task-build/driver -DTASK_TARGET="$flag" > .task-build/configure.log 2>&1; then tail -30 .task-build/configure.log; exit 1; fi
if ! cmake --build .task-build/driver --parallel 4 > .task-build/build.log 2>&1; then tail -30 .task-build/build.log; exit 1; fi
printf 'SOURCE_BUILD mode=%s OK\n' "${1:-baseline}"
