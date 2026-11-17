#!/bin/bash
set -euo pipefail
mkdir -p .task-build/objects
sources=$(find lib-macho/source stdxs/source -name '*.cpp' ! -path '*/UI/*' | sort)
input_hash=$({ find lib-macho stdxs -type f | sort | while IFS= read -r file; do shasum -a 256 "$file"; done; } | shasum -a 256)
if [[ ! -f .task-build/library.hash ]] || [[ $(cat .task-build/library.hash) != "$input_hash" ]]; then
  objects=()
  for file in $sources; do
    obj=".task-build/objects/$(echo "$file" | tr / _).o"
    c++ -std=c++17 -O0 -Ilib-macho/include -Istdxs/include -c "$file" -o "$obj"
    objects+=("$obj")
  done
  rm -f .task-build/libmacho.a
  ar rcs .task-build/libmacho.a "${objects[@]}"
  echo "$input_hash" > .task-build/library.hash
fi
c++ -std=c++17 -O0 -Ilib-macho/include -Istdxs/include task_support/baseline.cpp .task-build/libmacho.a -o .task-build/baseline
