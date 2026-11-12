#!/bin/sh
set -eu
# The caller starts at the repository root. No generated runner supplies counts.
for file in tests/qoa_search_contract.c tests/run-qoa-search.sh; do
    if [ ! -s "$file" ]; then echo "QOA_SEARCH_MISSING $file" >&2; exit 1; fi
done
build=$(mktemp -d)
trap 'rm -rf "$build"' EXIT HUP INT TERM
${CC:-cc} -std=c99 -O2 -Wall -Wextra -I. -Ichecks \
    checks/search_contract_main.c tests/qoa_search_contract.c -o "$build/search-contract"
"$build/search-contract"
