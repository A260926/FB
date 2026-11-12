#!/bin/bash
set -euo pipefail
build=$(mktemp -d)
trap 'rm -rf "$build"' EXIT
source checks/build-lib.sh
cc -std=c99 -O1 -I"$build/include" checks/baseline.c "$build"/*.o -lm -o "$build/witness"
"$build/witness"
cc -std=c99 -O1 -D_GNU_SOURCE -I"$build/include" test/parsing/yajl_test.c "$build"/*.o -lm -o "$build/old"
for fixture in array codepoints_from_unicode_org difficult_json_c_test_case; do
 for chunk in 1 7 31; do
  "$build/old" -b "$chunk" < "test/parsing/cases/$fixture.json" > "$build/result" 2>&1
  diff -u "test/parsing/cases/$fixture.json.gold" "$build/result"
 done
done
echo YAJL_OLD_PARSER_NINE_RUNS_OK
