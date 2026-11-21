#!/bin/bash
cd "$(dirname "$0")/.."
set -e
c++ -std=c++17 -O0 -g -I.task-build/core/include -Iinclude -Icode -I. -include code/PostProcessing/SceneBasisProcess.h -c test/unit/scene_basis_cases.cpp -o .task-build/sbc.o
c++ -std=c++17 -O0 -g -I.task-build/core/include -Iinclude -Icode -I. .task-build/dbg_main.cpp .task-build/sbc.o .task-build/scene_basis.o .task-build/core/lib/libassimp.a .task-build/core/contrib/zlib/libzlibstatic.a -o .task-build/dbg
