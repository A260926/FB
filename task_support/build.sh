#!/bin/bash
set -euo pipefail
mode=${1:-baseline}
cc=${CC:-cc}
cxx=${CXX:-c++}
root=$(pwd)
# Only this runner's own generated build products are cleared.
rm -rf .task-build/production
mkdir -p .task-build/production
(
  cd .task-build/production
  CC="$cc" CXX="$cxx" CFLAGS='-O0 -g -std=gnu99 -Wno-deprecated-declarations' bash ../../configure --disable-cxx --disable-shlib --enable-threads
  # Use the upstream preallocated-heap backend instead of host sbrk/mmap.
  sed -e 's/^#define HAVE_SBRK .*/#define HAVE_SBRK 0/' -e 's/^#define HAVE_MMAP .*/#define HAVE_MMAP 0/' conf.h > conf.tmp
  mv conf.tmp conf.h
  sed 's/(1024 \* 1024)/(64 * 1024 * 1024)/' settings.h > settings.tmp
  mv settings.tmp settings.h
  make -j2 libdmallocth.a
)
if [[ $mode == target ]]; then
  test -s allocation-contract.h
  test -s allocation-contract.c
  "$cc" -O0 -g -std=gnu99 -I.task-build/production -I. -DLOCK_THREADS=1 -include allocation-contract.h -c allocation-contract.c -o .task-build/production/allocation-contract.o
  ar rcs .task-build/production/libdmallocth.a .task-build/production/allocation-contract.o
fi
"$cc" -std=gnu99 -I.task-build/production -I. task_support/baseline.c .task-build/production/libdmallocth.a -pthread -o .task-build/baseline
