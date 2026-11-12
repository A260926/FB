#!/bin/bash
set -euo pipefail
b=$(mktemp -d); trap 'rm -rf "$b"' EXIT
bash checks/build.sh "$b/legacy" -Ichecks/legacy checks/legacy_main.cpp
"$b/legacy"
