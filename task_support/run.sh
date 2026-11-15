#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s tests/gltf_target.cpp; fi
mkdir -p .task-build
test -s src/ofbx.cpp
cc -O1 -c src/libdeflate.c -o .task-build/deflate.o
object_files=(.task-build/deflate.o)
for source_file in src/*.cpp; do
  test -s "$source_file"
  c++ -std=c++17 -O1 -I. -Isrc -c "$source_file" -o ".task-build/$(basename "$source_file" .cpp).o"
  object_files+=(".task-build/$(basename "$source_file" .cpp).o")
done
rm -f .task-build/libofbx.a
ar rcs .task-build/libofbx.a "${object_files[@]}"
c++ -std=c++17 -O1 -I. task_support/baseline.cpp .task-build/libofbx.a -o .task-build/baseline
if [[ $mode == target ]]; then
  test -s src/ofbx_gltf.h
test -s src/ofbx_gltf.cpp
test -s examples/export_pivot.cpp
c++ -std=c++17 -O1 -I. task_support/driver.c tests/gltf_target.cpp .task-build/ofbx_gltf.o .task-build/libofbx.a -o .task-build/target
c++ -std=c++17 -O1 -I. examples/export_pivot.cpp .task-build/ofbx_gltf.o .task-build/libofbx.a -o .task-build/example
rm -f .task-build/example.glb
.task-build/example
test -s .task-build/example.glb
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in factor motion skin; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
