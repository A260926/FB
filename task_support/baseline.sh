#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
build=$(mktemp -d)
trap 'rm -rf "$build"' EXIT
bash task_support/build.sh baseline "$build"
"$build/baseline"
"$build/upstream_tests" --plain --subset 'shadow_*' >"$build/legacy.log"
cat "$build/legacy.log"
grep -Eq '[1-9][0-9]*/[1-9][0-9]* \[PASS\].*shadow_blur' "$build/legacy.log"
grep -q '^0 failed,' "$build/legacy.log"
echo CANVAS_LEGACY_SHADOWS_OK
