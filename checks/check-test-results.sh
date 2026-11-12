#!/bin/bash
set -euo pipefail
log=${1:?output file required}
minimum=${2:?minimum test count required}
# A selected integration test binary must produce exactly one successful summary.
summary=$(sed -nE 's/^test result: ok\. ([0-9]+) passed; ([0-9]+) failed; ([0-9]+) ignored;.*$/\1 \2 \3/p' "$log")
[[ $summary =~ ^[0-9]+\ [0-9]+\ [0-9]+$ ]] || { echo 'missing or ambiguous successful test summary' >&2; exit 1; }
read -r passed failed ignored <<< "$summary"
(( passed >= minimum && failed == 0 && ignored == 0 )) || { echo "invalid test counts: passed=$passed failed=$failed ignored=$ignored" >&2; exit 1; }
shift 2
for name in "$@"; do
  grep -Fx "test $name ... ok" "$log" >/dev/null || { echo "required test did not pass: $name" >&2; exit 1; }
done
