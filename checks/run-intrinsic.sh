#!/usr/bin/env bash
set -euo pipefail
test -s checks/intrinsic.cpp
out=$(mktemp)
trap 'rm -f "$out"' EXIT
bash checks/build-run.sh checks/intrinsic.cpp | tee "$out"
for mark in 'PASS intrinsic-metric' 'PASS local-delaunay' 'PASS cotangent' 'PASS locks-and-repeat'; do
 grep -Fq "$mark" "$out"
done
bash checks/build-run.sh checks/baseline.cpp
echo 'OPENMESH_INTRINSIC_PASS'
