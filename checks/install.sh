#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
mkdir -p .check-build
make -j4 CC="${CC:-cc}" libbearssl.a testx509 > .check-build/build.log 2>&1 || { cat .check-build/build.log; exit 1; }
"${CC:-cc}" -std=c99 -Wall -Wextra -I inc checks/baseline.c libbearssl.a -o .check-build/baseline
printf '%s\n' BEARSSL_BUILD_OK
