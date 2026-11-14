#!/usr/bin/env bash
set -euo pipefail
mkdir -p .task-cache
BUILD=$(mktemp -d "$PWD/.task-cache/check-base.XXXXXX")
trap 'rm -rf "$BUILD"' EXIT
CXX=${CXX:-c++}
sources=()
for f in *.cpp; do [[ "$f" == main.cpp ]] || sources+=("$f"); done
"$CXX" -std=c++20 -O1 -I. task_support/baseline.cpp "${sources[@]}" -o "$BUILD/baseline"
"$CXX" -std=c++20 -O1 -I. main.cpp "${sources[@]}" -o "$BUILD/legacy-app"
(cd "$BUILD"; ./baseline; ./baseline --prepare; ./legacy-app triangle.obj; ./baseline --image)
