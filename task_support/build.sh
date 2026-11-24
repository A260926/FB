#!/bin/bash
set -euo pipefail
mkdir -p .task-build
cmake -S . -B .task-build/cmake -DCMAKE_BUILD_TYPE=Release -DGME_ZLIB=OFF -DGME_BUILD_SHARED=OFF -DGME_BUILD_STATIC=ON -DGME_BUILD_TESTING=OFF -DGME_BUILD_EXAMPLES=OFF -DUSE_GME_AY=OFF -DUSE_GME_GBS=OFF -DUSE_GME_GYM=OFF -DUSE_GME_HES=OFF -DUSE_GME_KSS=OFF -DUSE_GME_NSF=OFF -DUSE_GME_NSFE=OFF -DUSE_GME_SAP=OFF -DUSE_GME_VGM=OFF -DUSE_GME_SPC=ON > .task-build/configure.log 2>&1
cmake --build .task-build/cmake --target gme_static -j 2 > .task-build/build.log 2>&1 || { tail -30 .task-build/build.log; exit 1; }
bash task_support/cxx.sh -std=c++11 -O2 -Igme -Itask_support task_support/baseline.cpp .task-build/cmake/gme/libgme.a -o .task-build/baseline
