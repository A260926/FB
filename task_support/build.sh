#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
task_target=OFF
if [[ ${1:-baseline} == target ]]; then
 test -s src/libFLAC/stream_decoder_resumable.c
 task_target=ON
fi
cmake -S task_support/cmake -B .task-build/cmake -DTASK_TARGET="$task_target" -DCMAKE_BUILD_TYPE=Release -DBUILD_CXXLIBS=OFF -DBUILD_PROGRAMS=OFF -DBUILD_EXAMPLES=OFF -DBUILD_TESTING=OFF -DBUILD_DOCS=OFF -DINSTALL_MANPAGES=OFF -DINSTALL_PKGCONFIG_MODULES=OFF -DINSTALL_CMAKE_CONFIG_MODULE=OFF -DCMAKE_DISABLE_FIND_PACKAGE_Intl=ON -DCMAKE_DISABLE_FIND_PACKAGE_Iconv=ON -DWITH_OGG=OFF -DWITH_ASM=OFF -DENABLE_MULTITHREADING=OFF -DBUILD_SHARED_LIBS=OFF
cmake --build .task-build/cmake --clean-first --target FLAC -j2
test -s .task-build/cmake/upstream/src/libFLAC/libFLAC.a
