#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
test -s tests/context_node_contract.c
bash harness/build.sh
cc -std=c99 -Isrc tests/context_node_contract.c .build/libgumbo.a -o .build/context_node_contract
.build/context_node_contract
bash harness/baseline.sh
