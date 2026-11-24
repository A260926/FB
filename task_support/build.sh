#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
mkdir -p .task-build
root=$(pwd)
compiler=${CC:-cc}
platform=linux
case $(uname -s) in Darwin|FreeBSD|OpenBSD|NetBSD) platform=bsd;; esac
extra=
if [[ $mode == target ]]; then
  test -s context-identity.c
  test -s context-identity.h
  cat > .task-build/header_binding.c <<'C'
#include "lib.h"
#include "linearize.h"
#include "context-identity.h"
int task_header_binding(void) { return 0; }
C
  "$compiler" -std=gnu11 -I. -c .task-build/header_binding.c -o .task-build/header_binding.o
  "$compiler" -std=gnu11 -O1 -I. -c context-identity.c -o .task-build/context-identity.o
  extra="$root/.task-build/context-identity.o"
fi
rm -f sparse
# Remove only selected production objects and their generated archive.
make -f Makefile -f task_support/fresh-build.mk task-clean-objects OS="$platform" PKG_CONFIG=false HAVE_LIBXML= HAVE_SQLITE= HAVE_GTK= HAVE_LLVM= HAVE_BOOLECTOR= > .task-build/clean.log 2>&1 || { tail -30 .task-build/clean.log; exit 1; }
make -j4 sparse OS="$platform" CC="$compiler" PKG_CONFIG=false HAVE_LIBXML= HAVE_SQLITE= HAVE_GTK= HAVE_LLVM= HAVE_BOOLECTOR= CFLAGS=-O1 LDFLAGS="$extra" > .task-build/build.log 2>&1 || { tail -30 .task-build/build.log; exit 1; }
test -x sparse
