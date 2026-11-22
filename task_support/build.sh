#!/bin/bash
set -euo pipefail
mkdir -p .task-build
if [[ ! -f config.h || configure -nt config.mak2 ]]; then ./configure --extra-cflags=-O1; fi
make -j2 lib
if [[ ${1:-baseline} == target ]]; then
 test -s core/pool_undo.c
 test -s core/pool_undo.o
 ar -t liblsmash.a | grep -Eq '^pool_undo[.]o$'
fi
