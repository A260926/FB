#!/bin/bash
set -euo pipefail
mkdir -p .task-build
rm -f .task-build/*.o .task-build/libtask.a
flags=(-std=c11 -O1 -I. -Isrc -Itools/authoring -Itools/gltf2marrow -Itools/third_party)
if [[ $(uname -s) == Darwin && $(uname -m) == arm64 ]]; then flags+=(-arch x86_64); fi
for src in src/*.c tools/authoring/*.c tools/gltf2marrow/convert.c tools/gltf2marrow/joint_order.c tools/third_party/cgltf_impl.c; do
  extra=(-ffp-contract=off)
  case "$src" in
    *_sse2.c) extra=(-msse2 -mno-avx -mno-avx2 -mno-fma -mno-f16c);;
    *_avx2_fma.c) extra=(-mavx2 -mfma -mf16c);;
    *_avx2.c) extra=(-mavx2 -mf16c -mno-fma -ffp-contract=off);;
  esac
  cc "${flags[@]}" "${extra[@]}" -c "$src" -o ".task-build/$(basename "$src" .c).o"
done
ar rcs .task-build/libtask.a .task-build/*.o
cc "${flags[@]}" task_support/baseline.c .task-build/libtask.a -lm -o .task-build/baseline
cc "${flags[@]}" -DMRW_G2M_TESTDIR='".task-build"' tools/gltf2marrow/tests/test_gltf2marrow.c .task-build/libtask.a -lm -o .task-build/upstream-gltf
cc "${flags[@]}" tools/gltf2marrow/main.c .task-build/libtask.a -lm -o .task-build/gltf2marrow
