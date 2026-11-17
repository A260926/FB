#!/bin/bash
set -euo pipefail
mkdir -p .task-build
compiler=${CXX:-c++}
flags=(-std=c++17 -O0 -g0 -I.)
while IFS= read -r file; do
 [[ -s "$file" ]]
 obj=".task-build/$(echo "$file" | tr '/' '_').o"
 stamp=$( { cksum "$file"; find opensubdiv/sdc opensubdiv/vtr opensubdiv/far opensubdiv/bfr -name '*.h' -type f -exec cksum {} +; echo "$compiler ${flags[*]}"; } | cksum)
 if [[ ! -f "$obj" || ! -f "$obj.stamp" || $(cat "$obj.stamp") != "$stamp" ]]; then
  "$compiler" "${flags[@]}" -c "$file" -o "$obj"
  echo "$stamp" >"$obj.stamp"
 fi
done < task_support/original-cpp.txt
objects=()
while IFS= read -r file; do objects+=(".task-build/$(echo "$file" | tr '/' '_').o"); done < task_support/original-cpp.txt
"$compiler" "${flags[@]}" task_support/baseline.cpp "${objects[@]}" -o .task-build/baseline
