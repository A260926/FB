#!/bin/bash
set -euo pipefail
b=$(mktemp -d "${TMPDIR:-/tmp}/xatlas-check.XXXXXX")
trap 'rm -rf "$b"' EXIT
c++ -std=c++17 -O0 -DNDEBUG -pthread -DXATLAS_C_API=1 -Isource/xatlas task_support/baseline.cpp -o "$b/baseline"
"$b/baseline"
if [[ ${1:-baseline} == target ]]; then
 c++ -std=c++17 -O0 -DNDEBUG -pthread -DXATLAS_C_API=1 -Isource/xatlas -Itask_support task_support/main.cpp task_tests/target.cpp -o "$b/target"
 "$b/target" rectangular-ls stopping lscm-integration | tee "$b/output"
 awk -v style=C -v groups="rectangular-ls stopping lscm-integration" -f task_support/verify_output.awk "$b/output"
fi
