#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
if ! test -s tests/refresh_cases.c; then
 echo 'Missing or empty tests/refresh_cases.c' >&2
 exit 2
fi
b=$(mktemp -d .refresh-target.XXXXXX)
trap 'rm -rf "$b"' EXIT
source refresh_support/build.sh
build_case refresh_support/driver.c tests/refresh_cases.c
echo TARGET_COMPILED
set +e
"$b/case" | tee "$b/stdout.log"
statuses=("${PIPESTATUS[@]}")
status=${statuses[0]}
set -e
printf 'TARGET_EXECUTED exit=%s\n' "$status"
if [ "$status" -ne 0 ]; then exit "$status"; fi
if [ "${statuses[1]}" -ne 0 ]; then exit "${statuses[1]}"; fi
awk -f refresh_support/verify_output.awk "$b/stdout.log"
bash refresh_support/baseline.sh
bash refresh_support/old_test.sh
