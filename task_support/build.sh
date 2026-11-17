#!/bin/bash
set -euo pipefail
mkdir -p .task-build
rm -f .task-build/*.o
# Drop unreachable network entry points; every offline production function is compiled unchanged.
if [[ $(uname -s) == Darwin ]]; then
  task_link_strip=(-Wl,-dead_strip)
else
  task_link_strip=(-Wl,--gc-sections)
fi
sources=(src/MSFReader.cpp src/PDBParser.cpp src/PDB.cpp)
# New production units are included, with the existing network-only units excluded.
while IFS= read -r path; do
  case "$path" in src/MSFReader.cpp|src/PDBParser.cpp|src/PDB.cpp|src/Downloader.cpp|src/SymbolServer.cpp) ;;
    *) sources+=("$path") ;;
  esac
done < <(find src -maxdepth 1 -type f -name '*.cpp' | sort)
for path in "${sources[@]}"; do
 c++ -std=c++20 -O1 -pthread -ffunction-sections -fdata-sections -I. -Isrc -Iinclude -c "$path" -o ".task-build/$(basename "${path%.cpp}").o"
done
c++ -std=c++20 -O1 -pthread -ffunction-sections -fdata-sections -I. -Isrc -Iinclude task_support/baseline.cpp .task-build/*.o "${task_link_strip[@]}" -o .task-build/baseline
