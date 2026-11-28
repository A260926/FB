#!/bin/bash
set -euo pipefail
if [[ $1 == target ]]; then
  test -s src/marrow_mixed.c || { echo "required production source missing or empty: src/marrow_mixed.c" >&2; exit 1; }
fi
rm -rf .task-build
mkdir -p .task-build/objects
find src -type f -name '*.c' | LC_ALL=C sort > .task-build/sources.txt
test -s .task-build/sources.txt
while IFS= read -r source_file; do
  test -s "$source_file"
  case "$source_file" in
    *_avx2_fma.c) isa_flags=(-mavx2 -mfma -mf16c) ;;
    *_avx2.c) isa_flags=(-mavx2 -mf16c -mno-fma -ffp-contract=off) ;;
    *_sse2.c) isa_flags=(-msse2 -mno-avx -mno-avx2 -mno-fma -mno-f16c) ;;
    *) isa_flags=(-mno-avx -mno-avx2 -mno-fma -mno-f16c) ;;
  esac
  object_file=".task-build/objects/${source_file//\//_}.o"
  bash task_support/compiler.sh "${isa_flags[@]}" -c "$source_file" -o "$object_file"
done < .task-build/sources.txt
find .task-build/objects -type f -name '*.o' | LC_ALL=C sort > .task-build/objects.txt
test -s .task-build/objects.txt
objects=()
while IFS= read -r object_file; do objects+=("$object_file"); done < .task-build/objects.txt
ar rcs .task-build/libmarrow.a "${objects[@]}"
bash task_support/compiler.sh -c tools/authoring/mrw_authoring.c -o .task-build/authoring.o
bash task_support/compiler.sh task_support/baseline.c .task-build/authoring.o .task-build/libmarrow.a -lm -o .task-build/baseline
