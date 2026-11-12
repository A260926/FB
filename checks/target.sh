#!/bin/bash
set -euo pipefail
b=$(mktemp -d); trap 'rm -rf "$b"' EXIT
bash checks/build.sh "$b/contracts" checks/qa_main.cpp checks/contracts.cpp
"$b/contracts" > "$b/result"
cat "$b/result"
for group in analytic-paths knot-events termination; do
  grep -Eq "^PASS $group cases=[1-9][0-9]* assertions=[1-9][0-9]*$" "$b/result"
done
test "$(grep -c '^LNLIB_GEODESIC_GROUPS_COMPLETE$' "$b/result")" = 1
bash checks/baseline.sh
bash checks/legacy.sh
echo LNLIB_GEODESIC_TARGET_OK
