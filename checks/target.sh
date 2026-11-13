#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
b=$(mktemp -d); trap 'rm -rf "$b"' EXIT
bash checks/build.sh "$b/target" checks/target.cpp checks/driver.cpp
set +e
"$b/target" > "$b/stdout" 2> "$b/stderr"
status=$?
set -e
cat "$b/stdout"; cat "$b/stderr" >&2
test "$status" -eq 0
grep -Eq '^GROUP_OK layout [1-9][0-9]*$' "$b/stdout"
grep -Eq '^GROUP_OK aliases [1-9][0-9]*$' "$b/stdout"
grep -Eq '^GROUP_OK compat [1-9][0-9]*$' "$b/stdout"
grep -qx 'TARGET_OK' "$b/stdout"
bash checks/baseline.sh
