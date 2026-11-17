#!/bin/bash
set -euo pipefail
mkdir -p .task-build
cxx=${CXX:-c++}
flags=(-std=c++17 -O0 -I. -Isrc -DVIXL_CODE_BUFFER_MALLOC)
objects=()
for src in src/*.cc src/aarch64/*.cc; do
  case "$src" in
    src/code-buffer-vixl.cc|src/aarch64/assembler-aarch64.cc|src/aarch64/assembler-sve-aarch64.cc|src/aarch64/macro-assembler-aarch64.cc|src/aarch64/macro-assembler-sve-aarch64.cc|src/aarch64/cpu-aarch64.cc|src/aarch64/debugger-aarch64.cc|src/aarch64/simulator-aarch64.cc|src/aarch64/pointer-auth-aarch64.cc|src/aarch64/operands-aarch64.cc|src/aarch64/logic-aarch64.cc) continue;;
  esac
  obj=.task-build/${src//\//_}.o
  if [[ ! -f "$obj" || "$src" -nt "$obj" ]] || find src -name '*.h' -newer "$obj" -print -quit | grep -q .; then
    "$cxx" "${flags[@]}" -c "$src" -o "$obj"
  fi
  objects+=("$obj")
done
ar rcs .task-build/libanalysis.a "${objects[@]}"
"$cxx" "${flags[@]}" task_support/baseline.cpp .task-build/libanalysis.a -o .task-build/baseline
