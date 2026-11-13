#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
cc -std=c99 -Wall -Wextra -Iinclude task_support/baseline.c .push-build/libwavpack.a -lm -o .push-build/push-baseline
.push-build/push-baseline
