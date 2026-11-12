#!/bin/bash
set -euo pipefail
build=$(mktemp -d)
trap 'rm -rf "$build"' EXIT
bash checks/build-lib.sh "$build" -Ichecks checks/contract_main.c checks/pinned_contract.c
"$build/program" addresses packing compatibility
bash checks/baseline.sh
