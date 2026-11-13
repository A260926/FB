#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT
if [[ ${1:-baseline} == target ]]; then
 test -s tests/vsib_footprint_cases.c || { echo 'MISSING tests/vsib_footprint_cases.c' >&2; exit 2; }
 extras=()
 if [[ -d src/vsib ]]; then
  while IFS= read -r f; do extras+=("$f"); done < <(find src/vsib -name '*.c' -type f | sort)
 fi
 cc -std=c11 -O1 -Iinclude -I. src/BeaEngine.c "${extras[@]}" tests/vsib_footprint_cases.c -o "$out/footprint"
 "$out/footprint" | tee "$out/target.log"
 for group in encoding lanes addresses; do grep -qx "PASS $group" "$out/target.log"; done
 grep -qx 'VSIB_FOOTPRINT_OK' "$out/target.log"
fi
cc -std=c11 -O1 -Iinclude src/BeaEngine.c task_support/baseline.c -o "$out/baseline"
"$out/baseline"
