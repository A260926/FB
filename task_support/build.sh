#!/bin/bash
set -euo pipefail
build_target=OFF
if [[ ${1:-baseline} == target ]]; then build_target=ON; fi
case $(uname -m) in
 arm64|aarch64) max_isa=NEON ;;
 *) max_isa=SSE2 ;;
esac
cmake -S . -B .task-build -DCMAKE_BUILD_TYPE=Release \
 -DCMAKE_CXX_FLAGS_RELEASE='-O1 -DNDEBUG' -DCMAKE_C_FLAGS_RELEASE='-O1 -DNDEBUG' \
 -DEMBREE_MAX_ISA="$max_isa" -DEMBREE_TASKING_SYSTEM=INTERNAL \
 -DEMBREE_TUTORIALS=OFF -DEMBREE_ISPC_SUPPORT=OFF -DEMBREE_SYCL_SUPPORT=OFF \
 -DBUILD_TESTING=OFF -DEMBREE_STATIC_LIB=OFF -DEMBREE_MAX_INSTANCE_LEVEL_COUNT=4 \
 -DEMBREE_GEOMETRY_TRIANGLE=ON -DEMBREE_GEOMETRY_INSTANCE=ON \
 -DEMBREE_GEOMETRY_INSTANCE_ARRAY=ON -DEMBREE_GEOMETRY_USER=ON \
 -DEMBREE_GEOMETRY_QUAD=OFF -DEMBREE_GEOMETRY_CURVE=OFF \
 -DEMBREE_GEOMETRY_SUBDIVISION=OFF -DEMBREE_GEOMETRY_GRID=OFF \
 -DEMBREE_GEOMETRY_POINT=OFF -DEMBREE_RAY_PACKETS=ON \
 -DTASK_BUILD_TARGET="$build_target"
if [[ $build_target == ON ]]; then
 cmake --build .task-build --target task_baseline task_target task_example -j4
else
 cmake --build .task-build --target task_baseline -j4
fi
