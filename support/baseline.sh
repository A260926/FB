#!/bin/bash
set -euo pipefail
b=${CHECK_BUILD_DIR:-$(mktemp -d "$PWD/.build.XXXXXX")}
cmake -S support -B "$b" -DCMAKE_BUILD_TYPE=Debug
cmake --build "$b" --target baseline legacy_filter routing_baseline -j2
run=$(mktemp -d "$b/baseline-run.XXXXXX")
"$b/baseline" "$run/snapshot-db"
mkdir -p "$run/routing-db"
"$b/routing_baseline" "$run/routing-db"
LEVELDB_TESTS=FilterBlock "$b/legacy_filter" 2>&1 | tee "$b/legacy.log"
grep -q 'PASSED 3 tests' "$b/legacy.log"
printf 'LEGACY_FILTER_3_PASS\n'
