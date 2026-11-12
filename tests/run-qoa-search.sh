#!/bin/sh
set -eu
# Contract tests for the beam search encoder. The fixed entry point in
# checks/ owns the registry; this runner first executes it unchanged, then
# rebuilds and reruns the same six assertions under ASan/UBSan.

# 1. Fixed entry: compile checks/search_contract_main.c together with this
#    directory's test file and run the six named cases.
sh checks/verify-search.sh

build=$(mktemp -d)
trap 'rm -rf "$build"' EXIT HUP INT TERM

# 2. Same assertions under AddressSanitizer + UndefinedBehaviorSanitizer.
#    The extreme int16/int32 fixtures make any signed overflow fatal here.
echo "--- QOA search sanitizer run (address, undefined) ---"
${CC:-cc} -std=c99 -O1 -g \
	-fsanitize=address,undefined -fno-sanitize-recover=all \
	-Wall -Wextra -I. -Ichecks \
	checks/search_contract_main.c tests/qoa_search_contract.c \
	-o "$build/search-contract-san"
"$build/search-contract-san"
