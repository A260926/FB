#!/bin/bash
set -euo pipefail
if [[ ${1:-baseline} == target ]]; then
 test -s source/integrator/vertex-cm/vertex-cm.hpp
 test -s source/integrator/vertex-cm/vertex-cm.cpp
 test -s examples/vertex_cm.cpp
fi
cmake -S task_support -B .task-build/core -DCMAKE_BUILD_TYPE=Release > .task-configure.log 2>&1
cmake --build .task-build/core --target task_core -j 4 > .task-compile.log 2>&1
c++ -std=c++20 -pthread -I. -Ilib/glm -Ilib/nlohmann task_support/baseline.cpp .task-build/core/libtask_core.a -o .task-build/baseline
