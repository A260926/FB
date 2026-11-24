#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
mkdir -p .task-build/tmp
root=$(pwd)
compiler=${CC:-cc}
flags='-O1 -std=gnu89 -Wno-implicit-int -Wno-implicit-function-declaration -Wno-int-conversion -Wno-incompatible-pointer-types'
if ! test -x configure; then
  bash ./bootstrap > .task-build/bootstrap.log 2>&1
fi
if ! test -f config.status; then
  CC="$compiler" CFLAGS="$flags" ./configure --without-lcl --prefix="$root/.task-build/install" > .task-build/configure.log 2>&1
fi
"$compiler" -c task_support/yywrap.c -o .task-build/yywrap.o
extra="$root/.task-build/yywrap.o"
rs_cflags=""
if [[ $mode == target ]]; then
  rs_cflags="-DRS_WITH_RESULT_STATE"
  test -s src/resultState.c
  test -s src/Headers/resultState.h
  cat > .task-build/header_binding.c <<'C'
#include "splintMacros.nf"
#include "basic.h"
#include "resultState.h"
int task_header_binding(void) { return 0; }
C
  "$compiler" $flags -DHAVE_CONFIG_H -I. -Isrc -Isrc/Headers -c .task-build/header_binding.c -o .task-build/header_binding.o
  "$compiler" $flags -DHAVE_CONFIG_H -I. -Isrc -Isrc/Headers -c src/resultState.c -o .task-build/resultState.o
  extra="$extra $root/.task-build/resultState.o"
fi
# Explicitly relink the production program, including the new object in target mode.
rm -f src/splint
# Clear only this build's objects; retain generated grammar sources.
make -C src mostlyclean-compile > .task-build/clean.log 2>&1 || { tail -30 .task-build/clean.log; exit 1; }
make -C src -j4 CFLAGS="$flags $rs_cflags" LEXLIB="$extra" > .task-build/build.log 2>&1 || { tail -30 .task-build/build.log; exit 1; }
test -x src/splint
