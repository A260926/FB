#!/bin/bash
set -euo pipefail
mkdir -p .task-build
signature=$(find src/OpenMesh/Core src/OpenMesh/Tools/Utils task_support -type f ! -name 'run.sh' -exec cksum {} + | cksum)
if [[ ! -f .task-build/build.signature ]] || [[ $(cat .task-build/build.signature) != "$signature" ]]; then
  objects=()
  while IFS= read -r source; do
    object=".task-build/$(echo "$source" | tr / _).o"
    c++ -DOM_STATIC_BUILD -std=c++11 -O0 -w -Isrc -c "$source" -o "$object"
    objects+=("$object")
  done < <(find src/OpenMesh/Core -name '*.cc' ! -name '*T.cc' ! -path '*/Templates/*' | LC_ALL=C sort)
  ar rcs .task-build/core.a "${objects[@]}"
  c++ -DOM_STATIC_BUILD -std=c++11 -O0 -w -Isrc task_support/baseline.cpp .task-build/core.a -o .task-build/baseline
  printf '%s\n' "$signature" > .task-build/build.signature
fi
