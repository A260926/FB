#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
build=$(mktemp -d "${TMPDIR:-/tmp}/gnu_hash_rebuild.XXXXXX")
trap 'rm -rf "$build"' EXIT
c++ -std=c++17 -O2 -I. tests/gnu_hash_rebuild_contract.cpp -o "$build/gnu_hash_rebuild_contract"
"$build/gnu_hash_rebuild_contract" | tee "$build/output.txt"
named=$(awk '/^CASE_OK / {print $2}' "$build/output.txt" | sort -u | wc -l | tr -d ' ')
if [ "$named" -lt 8 ]; then
    echo "expected at least 8 named cases, saw $named" >&2
    exit 1
fi
grep -q '^ALL_CONTRACT_CASES_PASSED$' "$build/output.txt"
echo GNU_HASH_REBUILD_OK
