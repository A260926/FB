#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
: "${CC:=cc}"
mkdir -p .checks-build
# The upstream fallback header has its own BSD license and ships in this commit.
common=(-std=c99 -D_DEFAULT_SOURCE -D_GNU_SOURCE -O2 -I ebur128 -I ebur128/queue)
"$CC" "${common[@]}" -c ebur128/ebur128.c -o .checks-build/ebur128.o
