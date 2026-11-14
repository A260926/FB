#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
case "$mode" in
 baseline) ;;
 target)
  test -s tests/portal-collector.test.mjs
  task_output=$(mktemp)
  trap 'rm -f "$task_output"' EXIT
  set +e
  node task_support/driver.mjs >"$task_output" 2>&1
  status=$?
  set -e
  cat "$task_output"
  [[ $status == 0 ]] || exit "$status"
  for group in geometry membership integration; do
   grep -Eq "^PORTAL_GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$task_output"
  done
  grep -Eq '^PORTAL_TARGET_COMPLETE cases=[1-9][0-9]*$' "$task_output"
  ;;
 *) exit 64 ;;
esac
node task_support/baseline.mjs
