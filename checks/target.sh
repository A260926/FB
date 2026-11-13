#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
b=$(mktemp -d); trap 'rm -rf "$b"' EXIT
for bits in 16 32; do
 if test "$bits" = 32; then
  bash checks/build.sh "$b/target-$bits" checks/target.cpp checks/driver.cpp '-DImDrawIdx=unsigned int'
 else
  bash checks/build.sh "$b/target-$bits" checks/target.cpp checks/driver.cpp
 fi
 set +e
 "$b/target-$bits" > "$b/stdout-$bits" 2> "$b/stderr-$bits"
 status=$?
 set -e
 cat "$b/stdout-$bits"; cat "$b/stderr-$bits" >&2
 test "$status" -eq 0
 for group in replay large_mesh mutations; do
  grep -Eq "^GROUP_OK $group [1-9][0-9]*$" "$b/stdout-$bits"
 done
 grep -qx 'TARGET_OK' "$b/stdout-$bits"
 echo "ABI_OK $bits"
done
bash checks/baseline.sh
