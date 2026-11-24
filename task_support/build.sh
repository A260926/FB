#!/bin/bash
set -euo pipefail
unset HFUZZ_THREAD_NO
mode=${1:-baseline}
if [[ $mode == target ]]; then
  test -s libhfuzz/covmerge.h
  test -s libhfuzz/covmerge.c
fi
mkdir -p .task-build
cc_bin=${CC:-cc}
if [[ $(uname -s) == Darwin ]]; then arch=(-D_HF_ARCH_DARWIN); strip_flags=(-Wl,-dead_strip); else arch=(-D_GNU_SOURCE -D_HF_ARCH_LINUX); strip_flags=(-Wl,--gc-sections -ldl -lm); fi
for path in libhfuzz/instrument.c libhfuzz/memorycmp.c libhfcommon/files.c libhfcommon/util.c libhfcommon/log.c; do
 name=$(basename "$path" .c)
 "$cc_bin" -std=gnu11 -O1 -ffunction-sections -fdata-sections -I. "${arch[@]}" -c "$path" -o ".task-build/$name.o"
done
objects=(.task-build/instrument.o .task-build/memorycmp.o .task-build/files.o .task-build/util.o .task-build/log.o)
if [[ -e libhfuzz/covmerge.c ]]; then
  test -s libhfuzz/covmerge.h
  test -s libhfuzz/covmerge.c
  "$cc_bin" -std=gnu11 -O1 -I. "${arch[@]}" -include libhfuzz/covmerge.h -c libhfuzz/covmerge.c -o .task-build/covmerge.o
  objects+=(.task-build/covmerge.o)
fi
"$cc_bin" -std=gnu11 -O1 -I. "${arch[@]}" task_support/legacy.c "${objects[@]}" "${strip_flags[@]}" -pthread -o .task-build/legacy
