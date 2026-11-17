#!/bin/bash
set -euo pipefail
compiler=${CXX:-c++}
flags=(-std=c++17 -O0 -g0 -I.)
test -s opensubdiv/far/hierarchicalDisplacement.h
test -s opensubdiv/far/hierarchicalDisplacement.cpp
test -s examples/hierarchical_displacement.cpp
objects=()
while IFS= read -r file; do objects+=(".task-build/$(echo "$file" | tr '/' '_').o"); done < task_support/original-cpp.txt
"$compiler" "${flags[@]}" -c opensubdiv/far/hierarchicalDisplacement.cpp -o .task-build/hierarchicalDisplacement.o
objects+=(.task-build/hierarchicalDisplacement.o)
while IFS= read -r file; do
 case "$file" in opensubdiv/far/hierarchicalDisplacement.cpp) continue;; esac
 if ! grep -Fqx "$file" task_support/original-cpp.txt; then
  obj=".task-build/extra-$(echo "$file" | tr '/' '_').o"
  "$compiler" "${flags[@]}" -c "$file" -o "$obj"; objects+=("$obj")
 fi
done < <(find opensubdiv/sdc opensubdiv/vtr opensubdiv/far opensubdiv/bfr -name '*.cpp' -type f | LC_ALL=C sort)
"$compiler" "${flags[@]}" -x c++ task_support/driver.c task_tests/displacement.cpp -x none "${objects[@]}" -o .task-build/target
"$compiler" "${flags[@]}" examples/hierarchical_displacement.cpp "${objects[@]}" -o .task-build/example
output_dir=$(mktemp -d)
trap 'rm -rf "$output_dir"' EXIT
.task-build/example "$output_dir/sculpt.obj"
test -s "$output_dir/sculpt.obj"
grep -q '^v ' "$output_dir/sculpt.obj"
grep -q '^f ' "$output_dir/sculpt.obj"
