#!/usr/bin/env bash
set -euo pipefail
test -s verify/octasphere_geometry.cpp
test -s verify/check-geometry.sh
bash verify/check-geometry.sh
bash verify/baseline.sh
echo 'OCTASPHERE_GEOMETRY_PASS'
