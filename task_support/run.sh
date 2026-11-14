#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
if [ "${1:-baseline}" = target ]; then
 log=$(mktemp)
 trap 'rm -f "$log"' EXIT
 set +e
 bash task_support/run_inner.sh "$@" >"$log" 2>&1
 status=$?
 set -e
 cat "$log"
 [ "$status" = 0 ] || exit "$status"
 if grep -Eq '^[[:space:]]*--- (SKIP|FAIL): TestFrameSources([/ ]|$)' "$log"; then exit 4; fi
grep -Eq 'TASK_GROUP_DONE TestFrameSources checks=[1-9][0-9]*$' "$log"

if grep -Eq '^[[:space:]]*--- (SKIP|FAIL): TestFrameIdentity([/ ]|$)' "$log"; then exit 4; fi
grep -Eq 'TASK_GROUP_DONE TestFrameIdentity checks=[1-9][0-9]*$' "$log"

if grep -Eq '^[[:space:]]*--- (SKIP|FAIL): TestFrameLifecycle([/ ]|$)' "$log"; then exit 4; fi
grep -Eq 'TASK_GROUP_DONE TestFrameLifecycle checks=[1-9][0-9]*$' "$log"

else
 exec bash task_support/run_inner.sh "$@"
fi
