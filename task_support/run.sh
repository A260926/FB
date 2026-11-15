#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then
  test -s includes/ThinLens.h
  test -s src/ThinLens.cpp
  c++ -std=c++20 -D_BLINN_PHONG -D_PERSPECTIVE_TEXTURES -I. -Iincludes -Ilib/glm-0.9.8.2 -x c++ -fsyntax-only includes/ThinLens.h
  c++ -std=c++20 -D_BLINN_PHONG -D_PERSPECTIVE_TEXTURES -I. -Iincludes -Ilib/glm-0.9.8.2 -x c++ -fsyntax-only src/ThinLens.cpp
fi
if [[ $mode == target ]]; then test -s tests/thinlens_target.cpp; fi
mkdir -p .task-build
${CXX:-c++} -std=c++20 -O1 -Wno-return-type -D_BLINN_PHONG -D_PERSPECTIVE_TEXTURES -I. -Iincludes -Ilib/glm-0.9.8.2 $(find src -name "*.cpp" ! -name main.cpp -type f | LC_ALL=C sort) lib/EasyBMP_1.06/EasyBMP.cpp tests/baseline.cpp -o .task-build/baseline
if [[ $mode == target ]]; then
  ${CXX:-c++} -std=c++20 -O1 -Wno-return-type -D_BLINN_PHONG -D_PERSPECTIVE_TEXTURES -I. -Iincludes -Ilib/glm-0.9.8.2 $(find src -name "*.cpp" ! -name main.cpp -type f | LC_ALL=C sort) lib/EasyBMP_1.06/EasyBMP.cpp task_support/driver.c tests/thinlens_target.cpp -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in projection visibility integration lifetime; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
