#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
case ${1:-baseline} in
 baseline) node task_support/baseline.mjs ;;
 target)
  for f in src/renderer/coverage.js src/renderer/frame.js test/render-drawing.test.mjs examples/render-drawing.mjs; do test -s "$f"; done
  node --input-type=module -e 'await import("./src/renderer/coverage.js"); await import("./src/renderer/frame.js");'
  test -s task_support/require_render_api.mjs
  node task_support/require_render_api.mjs
  log=$(mktemp); image=$(mktemp); rm -f "$image"
  trap 'rm -f "$log" "$image"' EXIT
  node task_support/driver.mjs >"$log" 2>&1 || { cat "$log"; exit 1; }
  cat "$log"
  for g in geometry compositing timeline; do grep -Eq "^TASK_GROUP_DONE $g checks=[1-9][0-9]*$" "$log"; done
  grep -qx DRAWING_TARGET_DONE "$log"
  node examples/render-drawing.mjs task_support/drawing.ass 0.5 "$image"
  test -s "$image"
  node task_support/verify-pam.mjs "$image"
  node task_support/baseline.mjs ;;
 *) exit 2 ;;
esac
