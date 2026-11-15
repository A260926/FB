#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s tests/mesh_blend_target.c; fi
cmake_arch=()
cc_arch=()
if [[ $(uname -s) == Darwin && $(uname -m) == arm64 ]]; then cmake_arch=(-DCMAKE_OSX_ARCHITECTURES=x86_64); cc_arch=(-arch x86_64); fi
mkdir -p .task-build
if [[ $mode == target ]]; then test -s src/marrow_mesh_blend.c; test -s examples/mesh_blend.c; rm -f .task-build/CMakeFiles/marrow.dir/src/marrow_mesh_blend.c.o; fi
cmake -S . -B .task-build ${cmake_arch[@]+"${cmake_arch[@]}"} -DMRW_BUILD_TESTS=ON -DMRW_BUILD_TOOLS=OFF -DMRW_BUILD_EXAMPLES=OFF -DMRW_BUILD_DEMO=OFF -DMRW_BUILD_OZZ_BENCH=OFF
cmake --build .task-build --target marrow mrw_build test_blend test_format_pose test_blend_batch -j2
libs=(.task-build/tests/libmrw_build.a .task-build/tools/libmrw_bake.a .task-build/tools/libmrw_authoring.a .task-build/libmarrow.a)
cc ${cc_arch[@]+"${cc_arch[@]}"} -std=c11 -O2 -I. -Itools/authoring -Itools/bake -Isrc task_support/baseline.c "${libs[@]}" -lm -o .task-build/baseline
if [[ $mode == target ]]; then
  test -s .task-build/CMakeFiles/marrow.dir/src/marrow_mesh_blend.c.o
cc ${cc_arch[@]+"${cc_arch[@]}"} -std=c11 -O2 -I. -Itools/authoring -Itools/bake -Isrc tests/mesh_blend_target.c task_support/driver.c .task-build/CMakeFiles/marrow.dir/src/marrow_mesh_blend.c.o "${libs[@]}" -lm -o .task-build/target
cc ${cc_arch[@]+"${cc_arch[@]}"} -std=c11 -O2 -I. -Itools/authoring -Itools/bake -Isrc examples/mesh_blend.c .task-build/CMakeFiles/marrow.dir/src/marrow_mesh_blend.c.o "${libs[@]}" -lm -o .task-build/example
example_dir=$(mktemp -d)
.task-build/example "$example_dir"
test -s "$example_dir/mesh_blend.obj"
rm -r "$example_dir"
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in hierarchy batch contracts; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/tests/test_blend
.task-build/tests/test_format_pose
.task-build/tests/test_blend_batch
.task-build/baseline
