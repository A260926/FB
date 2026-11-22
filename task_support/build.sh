#!/bin/bash
set -euo pipefail
cmake -S . -B .task-build/cmake -DARIBCC_NO_RENDERER=ON -DARIBCC_BUILD_TESTS=OFF -DARIBCC_USE_FREETYPE=OFF -DARIBCC_USE_FONTCONFIG=OFF -DARIBCC_USE_CORETEXT=OFF -DARIBCC_USE_EMBEDDED_FREETYPE=OFF -DARIBCC_SHARED_LIBRARY=OFF -DCMAKE_BUILD_TYPE=Release
cmake --build .task-build/cmake --parallel 2
c++ -std=c++17 -O1 -Iinclude -I.task-build/cmake/include task_support/baseline.cpp .task-build/cmake/libaribcaption.a -o .task-build/baseline
