#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p .task-build
objects=()
for src in cs.c utils.c SStream.c MCInstrDesc.c MCRegisterInfo.c MCInst.c Mapping.c arch/RISCV/*.c; do
  obj=".task-build/${src//\//_}.o"
  cc -std=c99 -O1 -DCAPSTONE_USE_SYS_DYN_MEM -DCAPSTONE_HAS_RISCV -Iinclude -I. -c "$src" -o "$obj"
  objects+=("$obj")
done
rm -f .task-build/libcapstone.a
ar rcs .task-build/libcapstone.a "${objects[@]}"
cc -std=c99 -Iinclude task_support/baseline.c .task-build/libcapstone.a -o .task-build/baseline
