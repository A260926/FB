#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
test -f tests/clip_contract.c || { echo 'missing tests/clip_contract.c' >&2; exit 2; }
work=$(mktemp -d "${TMPDIR:-/tmp}/nsvg-target.XXXXXX")
trap 'rm -rf "$work"' EXIT
cc -std=c99 -O2 -DNDEBUG -Isrc -Isupport support/nanosvg_impl.c support/clip_main.c tests/clip_contract.c -lm -o "$work/clip-contract"
for group in geometry scopes coverage reuse; do
    "$work/clip-contract" "$group" | tee "$work/$group.log"
    grep -Eq "^CLIP_GROUP_OK $group cases=[1-9][0-9]* checks=[1-9][0-9]*$" "$work/$group.log"
done
bash support/baseline.sh --legacy
cc -std=c99 -O2 -Isrc support/nanosvg_impl.c example/clip_preview.c -lm -o "$work/preview"
mkdir -p .output
"$work/preview" .output/clip.ppm
test "$(head -c 13 .output/clip.ppm)" = "$(printf 'P6\n96 48\n255\n')"
test "$(wc -c < .output/clip.ppm | tr -d ' ')" = 13837
echo 'CLIP_TARGET_COMPLETE'
