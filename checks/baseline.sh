#!/bin/bash
set -euo pipefail
b=$(mktemp -d "${TMPDIR:-/tmp}/tinyspline.XXXXXX") || exit 1; trap 'rm -rf "$b"' EXIT
bash checks/build.sh "$b/baseline" checks/baseline.cpp
"$b/baseline"
