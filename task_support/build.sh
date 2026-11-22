#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
rm -rf .task-build/obj
mkdir -p .task-build/obj
if [[ ${1:-baseline} == target ]]; then
 test -s lib/region.c
 test -s include/theora/theoraregion.h
fi
flags=(-std=c99 -O2 -UNDEBUG -I. -Iinclude -Ilib -Itask_support/vendor/ogg/include)
if [[ ${1:-baseline} == target ]]; then flags+=(-include theora/theoraregion.h); fi
objects=()
# Generic C codec production path; instrumentation and disabled-encoder stub
# are optional alternatives. New top-level production .c files are included.
for src in lib/*.c; do
  name=${src##*/}; name=${name%.c}
  case "$name" in encoder_disabled|collect) continue ;; esac
  obj=".task-build/obj/$name.o"
  cc "${flags[@]}" -c "$src" -o "$obj"
  objects+=("$obj")
done
for name in bitwise framing; do
  obj=".task-build/obj/ogg_$name.o"
  cc "${flags[@]}" -c "task_support/vendor/ogg/src/$name.c" -o "$obj"
  objects+=("$obj")
done
archive_dir=$(mktemp -d .task-build/archive.XXXXXX)
ar rcs "$archive_dir/libcodec.a" "${objects[@]}"
mv "$archive_dir/libcodec.a" .task-build/libcodec.a
rmdir "$archive_dir"
