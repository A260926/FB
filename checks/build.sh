#!/bin/sh
set -eu
mkdir -p .build
${CC:-cc} -std=c99 -O2 -Wall -Wextra -I. checks/baseline.c -o .build/baseline
${CC:-cc} -std=c99 -O2 -I. examples/converter.c -o .build/converter
${CC:-cc} --version | head -n 1
