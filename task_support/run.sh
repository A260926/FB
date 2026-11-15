#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s checks/test_tjunctions.cpp; fi
mkdir -p .task-build
cmake -S . -B .task-build -DASSIMP_BUILD_ALL_IMPORTERS_BY_DEFAULT=OFF -DASSIMP_BUILD_ALL_EXPORTERS_BY_DEFAULT=OFF -DASSIMP_BUILD_OBJ_IMPORTER=ON -DASSIMP_BUILD_OBJ_EXPORTER=ON -DASSIMP_NO_EXPORT=OFF -DASSIMP_BUILD_TESTS=OFF -DASSIMP_BUILD_ASSIMP_TOOLS=OFF -DASSIMP_BUILD_SAMPLES=OFF -DASSIMP_BUILD_ZLIB=ON -DASSIMP_BUILD_DRACO=OFF -DBUILD_SHARED_LIBS=OFF -DASSIMP_INSTALL=OFF -DASSIMP_WARNINGS_AS_ERRORS=OFF -DASSIMP_IGNORE_GIT_HASH=ON -DFETCHCONTENT_FULLY_DISCONNECTED=ON -DCMAKE_C_COMPILER_LAUNCHER= -DCMAKE_CXX_COMPILER_LAUNCHER= -DCMAKE_BUILD_TYPE=Release '-DCMAKE_C_FLAGS=-include stdio.h' >.task-build/configure.log 2>&1 || { tail -30 .task-build/configure.log; exit 1; }
cmake --build .task-build --target assimp --parallel 4 >.task-build/compile.log 2>&1 || { tail -30 .task-build/compile.log; exit 1; }
assimp_lib=$(find .task-build -type f -name libassimp.a -print -quit)
zlib_lib=$(find .task-build -type f -name libzlibstatic.a -print -quit)
test -n "$assimp_lib"; test -n "$zlib_lib"
flags=(-std=c++17 -O0 -I. -Iinclude -Icode -I.task-build/include)
c++ "${flags[@]}" task_support/baseline.cpp "$assimp_lib" "$zlib_lib" -o .task-build/baseline
if [[ $mode == target ]]; then
  test -s code/PostProcessing/ResolveTJunctionsProcess.h
test -s code/PostProcessing/ResolveTJunctionsProcess.cpp
grep -q 'PostProcessing/ResolveTJunctionsProcess.cpp' code/CMakeLists.txt
new_object=.task-build/code/CMakeFiles/assimp.dir/PostProcessing/ResolveTJunctionsProcess.cpp.o
test -s "$new_object"
test -s examples/tjunctions.cpp
c++ "${flags[@]}" examples/tjunctions.cpp "$new_object" "$assimp_lib" "$zlib_lib" -o .task-build/example
rm -f .task-build/tjunction-result.obj .task-build/tjunction-result.mtl
.task-build/example .task-build/tjunction-result.obj
test -s .task-build/tjunction-result.obj
grep -q '^f ' .task-build/tjunction-result.obj
c++ "${flags[@]}" task_support/driver.c checks/test_tjunctions.cpp "$new_object" "$assimp_lib" "$zlib_lib" -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in boundary streams pipeline; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
