#!/bin/bash
set -euo pipefail
mkdir -p .task-build
c++ -std=c++17 -O1 -I nanovdb -I nanovdb/nanovdb/examples/ex_util -I . -include nanovdb/HostBuffer.h -x c++ -c nanovdb/nanovdb/examples/ex_raytrace_fog_volume/nanovdb.cu -o .task-build/fog.o
objects=(.task-build/fog.o)
while IFS= read -r path; do
  obj=".task-build/$(basename "$path").o"
  c++ -std=c++17 -O1 -I nanovdb -I . -c "$path" -o "$obj"
  objects+=("$obj")
done < <(find nanovdb/nanovdb/tools -maxdepth 1 -name '*.cc' -type f | sort)
rm -f .task-build/libfog.a
ar rcs .task-build/libfog.a "${objects[@]}"
c++ -std=c++17 -O1 -I nanovdb -I . task_support/baseline.cpp .task-build/libfog.a -o .task-build/baseline
