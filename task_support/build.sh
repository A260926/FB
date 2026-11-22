#!/bin/bash
set -euo pipefail
mkdir -p .task-build
cc_bin="${CC:-cc}"
flags=(-std=c99 -D_POSIX_C_SOURCE=200809L -O2 -I. -Icaption)
sources=(caption cea708 eia608 eia608_charmap mpeg scc srt utf8 vtt xds)
objects=()
for name in "${sources[@]}"; do
  "$cc_bin" "${flags[@]}" -c "src/$name.c" -o ".task-build/$name.o"
  objects+=(".task-build/$name.o")
done
"$cc_bin" "${flags[@]}" -x c -c src/eia608_from_utf8.c.cached -o .task-build/eia608_from_utf8.o
objects+=(.task-build/eia608_from_utf8.o)
if [[ ${1:-baseline} == target ]]; then
  for name in playout diff608; do
    test -s "src/$name.c"
    test -s "caption/$name.h"
    "$cc_bin" "${flags[@]}" -include caption/playout.h -include caption/diff608.h -c "src/$name.c" -o ".task-build/$name.o"
    objects+=(".task-build/$name.o")
  done
fi
rm -f .task-build/libcaption.a
ar rcs .task-build/libcaption.a "${objects[@]}"
"$cc_bin" "${flags[@]}" task_support/io_fixture.c .task-build/libcaption.a -lm -o .task-build/task_fixture
if [[ ${1:-baseline} != target ]]; then
  "$cc_bin" "${flags[@]}" task_support/baseline.c .task-build/libcaption.a -lm -o .task-build/baseline
fi
