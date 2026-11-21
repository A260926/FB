#!/bin/bash
set -euo pipefail
test -s include/FastBVH/EditableMesh.h
test -s src/EditableMesh.cpp
test -s examples/editable_mesh.cpp
c++ -std=c++14 -O2 -pthread -I. -Iinclude -include include/FastBVH/EditableMesh.h -x c++ task_support/driver.c tests/editable_mesh_cases.cpp src/EditableMesh.cpp -o .task-build/target
c++ -std=c++14 -O2 -pthread -I. -Iinclude examples/editable_mesh.cpp src/EditableMesh.cpp -o .task-build/editable_mesh
out_dir=$(mktemp -d)
trap 'rm -rf "$out_dir"' EXIT
.task-build/editable_mesh "$out_dir"
test -s "$out_dir/before.ppm"
test -s "$out_dir/after.ppm"
