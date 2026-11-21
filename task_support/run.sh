#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s tests/section_target.cpp; fi
targetflags=''
if [[ $mode == target ]]; then
 test -s includes/SectionPass.h
 test -s src/SectionPass.cpp
 targetflags='-include includes/SectionPass.h'
fi
rm -rf .task-build/obj
mkdir -p .task-build/obj
find src -name '*.cpp' ! -name main.cpp -type f | LC_ALL=C sort > .task-build/sources
test -s .task-build/sources
while IFS= read -r src; do
 obj=".task-build/obj/$src.o"
 mkdir -p "$(dirname "$obj")"
 c++ -std=c++20 -O1 -w -D_BLINN_PHONG -D_PERSPECTIVE_TEXTURES -I. -Iincludes -Ilib/glm-0.9.8.2 $targetflags -c "$src" -o "$obj"
done < .task-build/sources
c++ -std=c++20 -O1 -w -D_BLINN_PHONG -D_PERSPECTIVE_TEXTURES -I. -Iincludes -Ilib/glm-0.9.8.2 $targetflags -c lib/EasyBMP_1.06/EasyBMP.cpp -o .task-build/obj/easybmp.o
c++ -std=c++20 -O1 -w -D_BLINN_PHONG -D_PERSPECTIVE_TEXTURES -I. -Iincludes -Ilib/glm-0.9.8.2 $targetflags tests/baseline.cpp $(find .task-build/obj -name '*.o' -type f | LC_ALL=C sort) -o .task-build/baseline
if [[ $mode == target ]]; then
  c++ -std=c++20 -O1 -w -D_BLINN_PHONG -D_PERSPECTIVE_TEXTURES -I. -Iincludes -Ilib/glm-0.9.8.2 $targetflags tests/section_target.cpp task_support/driver.c $(find .task-build/obj -name '*.o' -type f | LC_ALL=C sort) -o .task-build/target
test -s examples/section_preview.cpp
c++ -std=c++20 -O1 -w -D_BLINN_PHONG -D_PERSPECTIVE_TEXTURES -I. -Iincludes -Ilib/glm-0.9.8.2 $targetflags examples/section_preview.cpp $(find .task-build/obj -name '*.o' -type f | LC_ALL=C sort) -o .task-build/example
rm -f .task-build/section.ppm
.task-build/example .task-build/section.ppm
test -s .task-build/section.ppm
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in winding projection composition; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
