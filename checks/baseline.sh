#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
b=$(mktemp -d); trap 'rm -rf "$b"' EXIT
bash checks/build.sh "$b/base" checks/baseline.cpp
"$b/base"
