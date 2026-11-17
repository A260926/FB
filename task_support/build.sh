#!/bin/bash
set -euo pipefail
mkdir -p .task-build
cc=${CC:-cc}
flags=(-std=c99 -O2 -DNDEBUG -I.)
if [[ $(uname -s) == Darwin ]]; then
 flags+=(-D__unix__ -D_DARWIN_C_SOURCE -include task_support/darwin_compat.h)
fi
"$cc" "${flags[@]}" -DLIBTTAr_OPT_DISABLE_SIMD_INTRINSICS -c src/build_lib.c -o .task-build/lib.o
"$cc" "${flags[@]}" src/build_cli.c .task-build/lib.o -pthread -o .task-build/ttaR
"$cc" -std=c99 -O2 -I. task_support/baseline.c .task-build/lib.o -o .task-build/baseline
