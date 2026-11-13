#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
for mutation in tails offscreen; do
 mkdir -p "$work/$mutation"
 cp -R src task_support task_tests test "$work/$mutation/"
 file="$work/$mutation/src/canvas_ity.hpp"
 if [ "$mutation" = tails ]; then
  sed 's/float weight_1 = alpha \/ divisor;/float weight_1 = 0.95f * alpha \/ divisor;/' "$file" >"$file.new"
 else
  sed 's/offset, brush ).a;/offset, brush ).a * ((x >= offset.x \&\& y >= offset.y \&\& x < offset.x + size_x \&\& y < offset.y + size_y) ? 1.0f : 0.0f);/' "$file" >"$file.new"
 fi
 if cmp -s "$file" "$file.new"; then echo "mutation not applied" >&2; exit 1; fi
 mv "$file.new" "$file"
 (
  cd "$work/$mutation"
  bash task_support/build.sh target "$work/$mutation/build"
 )
 set +e
 "$work/$mutation/build/target" kernel offscreen paint state >"$work/$mutation/stdout" 2>"$work/$mutation/stderr"
 status=$?
 set -e
 cat "$work/$mutation/stdout"
 cat "$work/$mutation/stderr" >&2
 test "$status" -eq 1
 grep -q '^ASSERT_FAIL ' "$work/$mutation/stderr"
 echo "MUTATION_${mutation}_CONTENT_REJECTED"
done
