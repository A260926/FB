#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then
  test -s tests/pts-playback.cjs
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  node task_support/check-sources.cjs
  test -s task_support/require_pts_playback.cjs
  node task_support/require_pts_playback.cjs
  test -s examples/pts-playback.cjs
  demo_log=$(mktemp)
  node examples/pts-playback.cjs >"$demo_log" 2>&1
  cat "$demo_log"
  grep -qx PTS_PLAYBACK_DEMO_OK "$demo_log"
  rm -f "$demo_log"
  set +e
  node task_support/driver.cjs >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in transport anchors playout; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
node task_support/baseline.cjs
node task_support/nonflat.cjs
node task_support/audio-ts-baseline.cjs
