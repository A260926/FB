#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s test/unit/morph_frames.cpp; fi
mkdir -p .task-build
export CCACHE_DISABLE=1
cmake -S . -B .task-build/lib -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF -DASSIMP_HUNTER_ENABLED=OFF -DASSIMP_BUILD_ZLIB=OFF -DASSIMP_BUILD_TESTS=OFF -DASSIMP_INSTALL=OFF -DASSIMP_BUILD_ASSIMP_TOOLS=OFF -DASSIMP_BUILD_SAMPLES=OFF -DASSIMP_BUILD_ALL_IMPORTERS_BY_DEFAULT=OFF -DASSIMP_BUILD_ALL_EXPORTERS_BY_DEFAULT=OFF -DASSIMP_BUILD_GLTF_IMPORTER=ON -DASSIMP_BUILD_GLTF_EXPORTER=ON -DASSIMP_BUILD_M3D_IMPORTER=OFF -DASSIMP_BUILD_M3D_EXPORTER=OFF -DASSIMP_BUILD_USD_IMPORTER=OFF -DASSIMP_BUILD_VRML_IMPORTER=OFF -DASSIMP_BUILD_DRACO=OFF -DASSIMP_WARNINGS_AS_ERRORS=OFF -DFETCHCONTENT_FULLY_DISCONNECTED=ON -DCMAKE_C_COMPILER_LAUNCHER= -DCMAKE_CXX_COMPILER_LAUNCHER=
cmake --build .task-build/lib --target assimp --parallel 2
c++ -std=c++17 -O2 -I.task-build/lib/include -Iinclude -I. task_support/baseline.cpp .task-build/lib/lib/libassimp.a -lz -o .task-build/baseline
if [[ $mode == target ]]; then
  c++ -std=c++17 -O2 -I.task-build/lib/include -Iinclude -I. -Icontrib/rapidjson/include -x c++ test/unit/morph_frames.cpp task_support/driver.c -x none .task-build/lib/lib/libassimp.a -lz -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in generation ownership gltf; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
