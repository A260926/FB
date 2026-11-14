#!/bin/bash
set -euo pipefail
mkdir -p .task-build
cp src/eia608_from_utf8.c.cached .task-build/eia608_from_utf8.c
sources=(src/caption.c src/cea708.c src/eia608.c src/eia608_charmap.c .task-build/eia608_from_utf8.c src/mpeg.c src/scc.c src/srt.c src/utf8.c src/vtt.c src/xds.c)
if [ -f src/scc_timeline.c ]; then sources+=(src/scc_timeline.c); fi
objects=()
for src in "${sources[@]}"; do
 obj=".task-build/$(basename "${src%.c}").o"
 cc -std=c99 -O1 -I. -Icaption -c "$src" -o "$obj"
 objects+=("$obj")
done
rm -f .task-build/libcaption.a
ar rcs .task-build/libcaption.a "${objects[@]}"
cc -std=c99 -O1 -I. -Icaption examples/scc2vtt.c .task-build/libcaption.a -lm -o .task-build/scc2vtt
