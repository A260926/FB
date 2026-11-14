#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s task_tests/deferred_warp.cpp; fi
mkdir -p .task-build/include/manifold
sed -e 's/@MANIFOLD_VERSION_MAJOR@/3/g' -e 's/@MANIFOLD_VERSION_MINOR@/5/g' -e 's/@MANIFOLD_VERSION_PATCH@/1/g' cmake/version.h.in > .task-build/include/manifold/version.h
for src in src/*.cpp; do
  obj=.task-build/${src//\//_}.o
  if [[ ! -f $obj || $src -nt $obj ]] || find src include -name '*.h' -newer "$obj" -print -quit | grep -q .; then
    c++ -std=c++17 -O0 -pthread -DMANIFOLD_PAR=-1 -Iinclude -I.task-build/include -Isrc -c "$src" -o "$obj"
  fi
done
rm -f .task-build/libmanifold.a
ar rcs .task-build/libmanifold.a .task-build/src_*.o
c++ -std=c++17 -O0 -pthread -DMANIFOLD_PAR=-1 -Iinclude -I.task-build/include -I. task_support/baseline.cpp .task-build/libmanifold.a -o .task-build/baseline
if [[ $mode == target ]]; then
  c++ -std=c++17 -O0 -pthread -DMANIFOLD_PAR=-1 -Iinclude -I.task-build/include -I. task_support/driver.c task_tests/deferred_warp.cpp .task-build/libmanifold.a -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in deferred ordering sharing; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
