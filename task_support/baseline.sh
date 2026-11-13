#!/usr/bin/env bash
set -euo pipefail
mkdir -p .baseline
${CC:-cc} -std=c99 -O2 -Isrc task_support/baseline.c src/libinjection_xss.c src/libinjection_html5.c -o .baseline/xss
.baseline/xss
