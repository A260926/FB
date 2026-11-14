#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p .task-build
objects=()
for src in dwarf/*.cc elf/*.cc; do
 obj=".task-build/${src//\//_}.o"
 if [[ ! -f $obj || $src -nt $obj ]] || find dwarf elf -name '*.hh' -newer "$obj" -print -quit 2>/dev/null | grep -q .; then
  "${CXX:-c++}" -std=c++11 -O0 -I. -Idwarf -Ielf -c "$src" -o "$obj"
 fi
 objects+=("$obj")
done
ar rcs .task-build/libreader.a "${objects[@]}"
