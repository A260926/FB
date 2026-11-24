#!/bin/bash
set -euo pipefail
mkdir -p .task-build/obj
sources=(src/core/*.cpp src/audiosource/wav/soloud_wav.cpp src/audiosource/wav/soloud_wavstream.cpp src/audiosource/wav/dr_impl.cpp src/backend/null/soloud_null.cpp)
for src in "${sources[@]}"; do
 obj=.task-build/obj/$(basename "$src").o
 if [[ ! -f $obj || $src -nt $obj ]] || find include -type f -newer "$obj" -print -quit | read -r _; then
  c++ -std=c++17 -O2 -DWITH_NULL -Iinclude -c "$src" -o "$obj"
 fi
done
src=src/audiosource/wav/stb_vorbis.c
obj=.task-build/obj/stb_vorbis.o
if [[ ! -f $obj || $src -nt $obj ]]; then cc -O2 -Iinclude -c "$src" -o "$obj"; fi
rm -f .task-build/libsoloud.a
ar rcs .task-build/libsoloud.a .task-build/obj/*.o
c++ -std=c++17 -O2 -Iinclude -Itask_support task_support/baseline.cpp .task-build/libsoloud.a -pthread -o .task-build/baseline
