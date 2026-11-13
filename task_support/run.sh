#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT
sources=(pe-parser-library/src/buffer.cpp pe-parser-library/src/parse.cpp pe-parser-library/src/unicode_codecvt.cpp)
if [[ ${1:-baseline} == target ]]; then
 test -s tests/guard_coverage_cases.cpp || { echo 'MISSING tests/guard_coverage_cases.cpp' >&2; exit 2; }
 extras=()
 if [[ -d pe-parser-library/src/guard ]]; then
  while IFS= read -r f; do extras+=("$f"); done < <(find pe-parser-library/src/guard -name '*.cpp' -type f | sort)
 fi
 c++ -std=c++17 -Ipe-parser-library/include -I. "${sources[@]}" "${extras[@]}" tests/guard_coverage_cases.cpp -o "$out/guard"
 "$out/guard" | tee "$out/target.log"
 for group in versions policies slots; do grep -qx "PASS $group" "$out/target.log"; done
 grep -qx 'GUARD_COVERAGE_OK' "$out/target.log"
fi
c++ -std=c++17 -Ipe-parser-library/include task_support/baseline.cpp "${sources[@]}" -o "$out/baseline"
"$out/baseline"
