#!/bin/bash
set -euo pipefail
b=$(mktemp -d); trap 'rm -rf "$b"' EXIT
bash checks/build.sh "$b/contracts" checks/qa_main.cpp checks/contracts.cpp
"$b/contracts"
bash checks/baseline.sh
