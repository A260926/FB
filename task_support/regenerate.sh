#!/bin/bash
# Optional author/developer regeneration; normal builds use delivered C/headers.
set -euo pipefail
cd "$(dirname "$0")/.."
for kind in convert fsize msize; do
  m4 -DSRCDIR=libelf "libelf/libelf_${kind}.m4" > "task_support/generated/libelf_${kind}.c"
done
m4 -I common/sys common/sys/elfdefinitions.m4 > task_support/include/sys/elfdefinitions.h
