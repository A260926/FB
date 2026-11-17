#!/bin/bash
set -euo pipefail
clang -target x86_64-unknown-linux-gnu -c task_support/samples/guarded.s -o .task-build/guarded.o
.task-build/objconv -fnasm .task-build/guarded.o .task-build/guarded.asm
cat .task-build/guarded.asm
printf '\nBASELINE_OK: production ELF64 disassembly completed\n'
