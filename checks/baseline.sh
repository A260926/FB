#!/bin/bash
set -euo pipefail
b=$(mktemp -d); trap 'rm -rf "$b"' EXIT
bash checks/build.sh "$b/baseline" checks/baseline.cpp
"$b/baseline"
