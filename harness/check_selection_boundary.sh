#!/usr/bin/env bash
set -euo pipefail
b=$(mktemp -d "${TMPDIR:-/tmp}/ovm-selection-boundary.XXXXXXXX")
trap 'rm -rf "$b"' EXIT
cmake -S harness -B "$b" -DCMAKE_BUILD_TYPE=Debug
cmake --build "$b" --target check_selection_boundary -j 4
"$b/check_selection_boundary"
