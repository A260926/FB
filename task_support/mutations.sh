#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
for mutation in winding neighbour; do
 mkdir -p "$work/$mutation"
 cp -R Source Include task_support task_tests "$work/$mutation/"
 if [ "$mutation" = winding ]; then
  file="$work/$mutation/Source/sweep.c"
  sed 's/return (n >= 2) || (n <= -2);/return (n != 0);/' "$file" >"$file.new"
 else
  file="$work/$mutation/Source/tess.c"
  sed 's/return edge->Rface->n;/return edge->Lface->n;/' "$file" >"$file.new"
 fi
 if cmp -s "$file" "$file.new"; then echo "mutation not applied" >&2; exit 1; fi
 mv "$file.new" "$file"
 (cd "$work/$mutation"; bash task_support/build.sh target "$work/$mutation/build")
 set +e
 "$work/$mutation/build/target" winding connectivity boundary projection >"$work/$mutation/stdout" 2>"$work/$mutation/stderr"
 status=$?
 set -e
 cat "$work/$mutation/stdout"; cat "$work/$mutation/stderr" >&2
 test "$status" -eq 1
 grep -q '^ASSERT_FAIL ' "$work/$mutation/stderr"
 echo "MUTATION_${mutation}_CONTENT_REJECTED"
done
