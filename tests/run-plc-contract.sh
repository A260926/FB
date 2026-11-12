#!/bin/sh
# Build and run the G.722 fill-in contract tests.
# Requires at least 12 registered cases; a zero-case or skipped run fails.
set -eu
test -s tests/plc_contract.c
test -s checks/plc_contract.h
test -s checks/plc_contract_main.c
build=$(mktemp -d)
trap 'rm -rf "$build"' EXIT HUP INT TERM
${CC:-cc} -std=c99 -O2 -I. -Ichecks checks/plc_contract_main.c tests/plc_contract.c g722_encode.c g722_decode.c -lm -o "$build/contract"
"$build/contract"
