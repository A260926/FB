#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."

# In restricted sandboxes the default mktemp template directory can be
# unwritable while $TMPDIR points at a writable per-session directory. Prefer a
# local wrapper, inherited by every child script (including checks/baseline.sh).
if ! probe_dir=$(mktemp -d 2>/dev/null); then
    export PATH="$PWD/checks/sandbox-bin:$PATH"
else
    rm -rf "$probe_dir"
fi
# Mutation source copies need an absolute repository root independent of the
# temporary working directory the nested driver runs from.
export MUT_REPO_ROOT="$PWD"

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
