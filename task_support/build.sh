#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
rm -rf .task-build
mkdir -p .task-build/obj
for source in libelf/*.c task_support/generated/*.c; do
  object=".task-build/obj/$(basename "${source%.c}").o"
  "${CC:-cc}" -std=c11 -O0 -g -D_DEFAULT_SOURCE -I. -Ilibelf -Icommon -Itask_support/include -c "$source" -o "$object"
done
ar rcs .task-build/libelf.a .task-build/obj/*.o
"${CC:-cc}" -std=c11 -I. -Ilibelf -Icommon -Itask_support/include task_support/make_samples.c -o .task-build/make_samples
"${CC:-cc}" -std=c11 -I. -Ilibelf -Icommon -Itask_support/include task_support/baseline.c .task-build/libelf.a -o .task-build/baseline
