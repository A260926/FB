#!/bin/bash
set -euo pipefail
cmake -S . -B .task-build/core -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_FLAGS=-Dfdopen=fdopen -DBUILD_SHARED_LIBS=OFF -DASSIMP_BUILD_TESTS=OFF -DASSIMP_BUILD_ASSIMP_TOOLS=OFF -DASSIMP_INSTALL=OFF -DASSIMP_BUILD_DOCS=OFF -DASSIMP_BUILD_SAMPLES=OFF -DASSIMP_WARNINGS_AS_ERRORS=OFF -DASSIMP_BUILD_ALL_IMPORTERS_BY_DEFAULT=OFF -DASSIMP_BUILD_OBJ_IMPORTER=ON -DASSIMP_BUILD_USD_IMPORTER=OFF -DASSIMP_BUILD_NO_C4D_IMPORTER=ON -DASSIMP_NO_EXPORT=ON -DASSIMP_BUILD_ZLIB=ON -DASSIMP_BUILD_DRACO=OFF -DASSIMP_HUNTER_ENABLED=OFF -DFETCHCONTENT_FULLY_DISCONNECTED=ON
cmake --build .task-build/core --target assimp -j 6
c++ -std=c++17 -O0 -I.task-build/core/include -Iinclude -Icode -I. task_support/baseline.cpp .task-build/core/lib/libassimp.a .task-build/core/contrib/zlib/libzlibstatic.a -o .task-build/baseline
