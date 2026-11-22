#!/bin/bash
set -euo pipefail
mkdir -p .task-build/obj
sources=(src/*.cpp vendor/libebml/src/*.cpp)
objects=()
for source in "${sources[@]}"; do
 object=".task-build/obj/${source//\//_}.o"
 if [[ ! -f "$object" || "$source" -nt "$object" ]] || find matroska vendor/libebml/ebml -name '*.h' -newer "$object" -print -quit | grep -q .; then
  c++ -std=c++17 -O1 -I. -Ivendor/libebml -Ivendor/utfcpp/source -Itask_support/include -c "$source" -o "$object"
 fi
 objects+=("$object")
done
rm -f .task-build/libmedia.a
ar rcs .task-build/libmedia.a "${objects[@]}"
