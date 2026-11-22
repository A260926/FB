#!/bin/bash
set -euo pipefail
mkdir -p .task-build
find libspeex -maxdepth 1 -type f -name '*.c' | LC_ALL=C sort | while IFS= read -r src; do
  case "$src" in
    libspeex/testenc.c|libspeex/testenc_wb.c|libspeex/testenc_uwb.c|libspeex/smallft.c|libspeex/kiss_fft.c|libspeex/kiss_fftr.c|libspeex/fftwrap.c|libspeex/vorbis_psy.c) ;;
    *) test -s "$src"; printf '%s\n' "$src" ;;
  esac
done > .task-build/sources.txt
test -s .task-build/sources.txt
find include libspeex task_support/include -type f \( -name '*.c' -o -name '*.h' \) | LC_ALL=C sort | while IFS= read -r f; do cksum "$f"; done > .task-build/source-stamp.new
cksum task_support/build.sh >> .task-build/source-stamp.new
rebuild=1
if [[ -f .task-build/source-stamp ]] && cmp -s .task-build/source-stamp.new .task-build/source-stamp; then rebuild=0; fi
for numeric in float fixed; do
  if [[ $numeric == float ]]; then definition=FLOATING_POINT; else definition=FIXED_POINT; fi
  mkdir -p ".task-build/$numeric"
  if [[ $rebuild == 1 || ! -s .task-build/$numeric/libspeex.a ]]; then
  : > ".task-build/$numeric/objects.txt"
  while IFS= read -r src; do
    obj=".task-build/$numeric/$(basename "${src%.c}").o"
    cc -std=c99 -O2 -D"$definition" -DVAR_ARRAYS -DEXPORT= -Iinclude -Itask_support/include -Ilibspeex -c "$src" -o "$obj"
    printf '%s\n' "$obj" >> ".task-build/$numeric/objects.txt"
  done < .task-build/sources.txt
  rm -f ".task-build/$numeric/libspeex.a"
  # The list is nonempty and consists only of controlled libspeex basenames.
  xargs ar rcs ".task-build/$numeric/libspeex.a" < ".task-build/$numeric/objects.txt"
  fi
  cc -std=c99 -O2 -Iinclude -Itask_support/include task_support/reset_probe.c ".task-build/$numeric/libspeex.a" -lm -o ".task-build/$numeric/reset-probe"
done
mv .task-build/source-stamp.new .task-build/source-stamp
