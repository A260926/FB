#!/bin/bash
set -euo pipefail
if [[ ${1:-baseline} == target ]]; then
 test -s include/luz/Renderer/ControlVariate.hpp
 test -s src/renderer/ControlVariate.cpp
 test -s examples/control_variate.cpp
fi
cmake -S . -B .task-build/core -DCMAKE_BUILD_TYPE=Debug -DLUZ_ENABLE_LTO=OFF -DLUZ_NATIVE_OPTIMIZATIONS=OFF > .task-configure.log 2>&1
cmake --build .task-build/core --target luz_core -j 4 > .task-compile.log 2>&1
c++ -std=c++20 -pthread -I. -Iinclude/luz task_support/baseline.cpp .task-build/core/libluz_core.a -o .task-build/baseline
