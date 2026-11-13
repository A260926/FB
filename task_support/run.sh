#!/bin/bash
set -euo pipefail
b=$(mktemp -d "${TMPDIR:-/tmp}/osd-check.XXXXXX")
trap 'rm -rf "$b"' EXIT
for f in opensubdiv/sdc/*.cpp opensubdiv/vtr/*.cpp opensubdiv/far/*.cpp; do
 o=${f//\//_}; c++ -std=c++17 -O0 -DNDEBUG -I. -c "$f" -o "$b/$o.o"
done
ar rcs "$b/far.a" "$b"/*.o
c++ -std=c++17 -O0 -I. task_support/baseline.cpp "$b/far.a" -o "$b/baseline"
"$b/baseline"
if [[ ${1:-baseline} == target ]]; then
 c++ -std=c++17 -O0 -I. -Itask_support task_support/main.cpp task_tests/target.cpp "$b/far.a" -o "$b/target"
 "$b/target" masks propagated-sensitivity ownership | tee "$b/output"
 awk -v style=C -v groups="masks propagated-sensitivity ownership" -f task_support/verify_output.awk "$b/output"
fi
