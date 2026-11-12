#!/bin/sh
set -eu
test -s tests/plc_contract.c
test -s tests/run-plc-contract.sh
build=$(mktemp -d)
trap 'rm -rf "$build"' EXIT HUP INT TERM
${CC:-cc} -std=c99 -O2 -I. -Ichecks checks/plc_contract_main.c tests/plc_contract.c g722_encode.c g722_decode.c -lm -o "$build/contract"
"$build/contract"
