#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s tests/skin_surface.cpp; fi
if [[ $mode == target ]]; then
 test -s demo/skin_surface.h
 test -s demo/skin_surface.cpp
 printf '#include "demo/skin_surface.h"\n' | ${CXX:-c++} -std=c++17 -DIQM_HEADLESS -I. -x c++ -fsyntax-only -
fi
mkdir -p .task-build
cxx=${CXX:-c++}
"$cxx" -std=c++17 -O2 -DIQM_HEADLESS -I. -c demo/demo.cpp -o .task-build/demo.o
"$cxx" -std=c++17 -O2 -DIQM_HEADLESS -I. -c task_support/current.cpp -o .task-build/current.o
"$cxx" -std=c++17 -O2 -DIQM_HEADLESS -I. -x c++ -c task_support/driver.c -o .task-build/driver.o
printf '%s\n' .task-build/demo.o > .task-build/production.rsp
extra_number=0
while IFS= read -r source; do
  case "$source" in demo/demo.cpp|demo/texture.cpp|demo/gpu-demo.cpp) continue;; esac
  extra_number=$((extra_number+1))
  object=".task-build/extra-$extra_number.o"
  "$cxx" -std=c++17 -O2 -DIQM_HEADLESS -I. -c "$source" -o "$object"
  printf '%s\n' "$object" >> .task-build/production.rsp
done < <(find demo -name '*.cpp' -type f | LC_ALL=C sort)
"$cxx" @.task-build/production.rsp .task-build/current.o -o .task-build/current
if [[ $mode == target ]]; then
  "$cxx" -std=c++17 -O2 -DIQM_HEADLESS -I. -c tests/skin_surface.cpp -o .task-build/target.o
"$cxx" @.task-build/production.rsp .task-build/target.o .task-build/driver.o -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in surface topology pipeline; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/current
