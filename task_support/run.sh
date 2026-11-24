#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s checks/target.cpp; fi
mkdir -p .task-build
for unit in spirv_cross spirv_parser spirv_cross_parsed_ir spirv_cfg spirv_glsl spirv_hlsl spirv_msl; do
  c++ -std=c++11 -O0 -I. -c "$unit.cpp" -o ".task-build/$unit.o"
done
c++ -std=c++11 -O0 -I. task_support/baseline.cpp .task-build/spirv_cross.o .task-build/spirv_parser.o .task-build/spirv_cross_parsed_ir.o .task-build/spirv_cfg.o .task-build/spirv_glsl.o -o .task-build/baseline
c++ -std=c++11 -O0 task_support/bridge.cpp -o .task-build/bridge
if [[ $mode == target ]]; then
  c++ -std=c++11 -x c++ -O0 -I. -Itask_support -c task_support/driver.c -o .task-build/driver.o
c++ -std=c++11 -O0 -I. -Itask_support checks/target.cpp .task-build/driver.o .task-build/spirv_cross.o .task-build/spirv_parser.o .task-build/spirv_cross_parsed_ir.o .task-build/spirv_cfg.o .task-build/spirv_glsl.o .task-build/spirv_hlsl.o .task-build/spirv_msl.o -o .task-build/target
  output_log=$(mktemp .task-build/target-log.XXXXXX)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in pairing calls compat; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
rm -f .task-build/scalar.glsl .task-build/image-array.glsl .task-build/two-axis.glsl .task-build/scalar.slots .task-build/image-array.slots .task-build/two-axis.slots .task-build/bridge-scalar.cpp .task-build/bridge-image.cpp .task-build/bridge-two-axis.cpp .task-build/bridge-scalar .task-build/bridge-image .task-build/bridge-two-axis
.task-build/baseline .task-build
.task-build/bridge .task-build/scalar.glsl .task-build/bridge-scalar.cpp 1 1 .task-build/scalar.slots
c++ -std=c++11 -O0 .task-build/bridge-scalar.cpp -o .task-build/bridge-scalar
.task-build/bridge-scalar
.task-build/bridge .task-build/image-array.glsl .task-build/bridge-image.cpp 3 1 .task-build/image-array.slots
c++ -std=c++11 -O0 .task-build/bridge-image.cpp -o .task-build/bridge-image
.task-build/bridge-image
if [[ -s .task-build/two-axis.glsl ]]; then
  .task-build/bridge .task-build/two-axis.glsl .task-build/bridge-two-axis.cpp 2 3 .task-build/two-axis.slots
  c++ -std=c++11 -O0 .task-build/bridge-two-axis.cpp -o .task-build/bridge-two-axis
  .task-build/bridge-two-axis
fi
