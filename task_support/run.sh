#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s tests/lighting_target.cpp; fi
bash task_support/build.sh
if [[ $mode == target ]]; then
  c++ -std=c++11 -I. -Iincludes -Itask_support/vendor -include glm/gtc/quaternion.hpp -Wno-c++11-narrowing -D_BLINN_PHONG -D_PERSPECTIVE_TEXTURES -x c++ task_support/driver.c tests/lighting_target.cpp -x none .task-build/*.o -lm -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in lighting projection lifetime; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
