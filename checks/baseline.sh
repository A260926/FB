#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
bash checks/build.sh
"${CC:-cc}" -std=c99 -O2 -I ebur128 checks/baseline.c .checks-build/ebur128.o -lm -o .checks-build/baseline
.checks-build/baseline
