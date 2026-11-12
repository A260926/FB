#!/bin/bash
set -euo pipefail
build=$(mktemp -d)
trap 'rm -rf "$build"' EXIT
cc -std=c99 -O1 -I. -Isrc -Ichecks schrift.c $(test ! -f schrift_cff.c || printf schrift_cff.c) checks/contract_main.c checks/cff_contract.c -lm -o "$build/contract"
"$build/contract" outlines subr-hints invalid
bash checks/baseline.sh
