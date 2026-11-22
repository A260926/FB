#!/bin/bash
set -euo pipefail
mode=${1:-baseline}
includes="-Iinclude -Ilib -Itask_support/include -Itask_support/vendor/ogg/include -I."
extra=""
if [[ $mode == target ]]; then
 test -s lib/rewind_cache.h
 test -s lib/rewind_cache.c
 extra="-include lib/rewind_cache.h"
fi
objdir=.task-build/obj-$mode
mkdir -p "$objdir"
objects=""
for file in lib/*.c task_support/vendor/ogg/src/bitwise.c task_support/vendor/ogg/src/framing.c; do
 case "$file" in lib/psytune.c|lib/tone.c|lib/barkmel.c) continue;; esac
 object="$objdir/$(basename "${file%.c}").o"
 cc -std=c99 -O1 -DHAVE_ALLOCA_H $includes $extra -c "$file" -o "$object"
 objects="$objects $object"
done
test -n "$objects"
rm -f .task-build/libcodec.a
ar rcs .task-build/libcodec.a $objects
