#!/bin/bash
set -euo pipefail
if [[ ! -s .checks-build-dir ]] || [[ ! -d "$(cat .checks-build-dir)" ]]; then bash checks/setup.sh; fi
b=$(cat .checks-build-dir)
cmake -S checks -B "$b" -DCMAKE_BUILD_TYPE=Release
cmake -E rm -f "$b/CMakeFiles/contracts.dir/contracts.cpp.o"
cmake --build "$b" --target contracts -j2
"$b/contracts"
bash checks/baseline.sh
