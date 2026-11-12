#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
build=$(mktemp -d)
trap 'rm -rf "$build"' EXIT
cc -std=c99 -O2 -DED25519_NO_SEED -DED25519_H2C_TESTING -Isrc src/*.c tests/h2c_contract.c baseline/target-main.c -o "$build/target"
"$build/target"
bash baseline/run.sh
