#!/usr/bin/env bash
# Verify the user-visible transcript produced by examples/covmerge_replay.
set -euo pipefail

transcript=${1:?usage: covmerge_transcript_check.sh TRANSCRIPT}
test -s "$transcript"

grep -q '^module=A .* positions=4$' "$transcript"
grep -q '^module=B .* positions=2$' "$transcript"
grep -q '^round=1 result=recorded-before-advance epoch=1$' "$transcript"
grep -q '^advance old_epoch=1 new_epoch=2 result=ok shared_counts_cleared=true$' "$transcript"
grep -q '^round=1-late result=discarded stale_epoch=1 current_epoch=2$' "$transcript"
grep -q '^round=2 result=saturated-merge epoch=2$' "$transcript"
grep -q 'values=\[30,60,90,120\]' "$transcript"
grep -q 'values=\[0,0,0,0\]' "$transcript"
grep -q 'values=\[255,201,255,255\]' "$transcript"
grep -q 'values=\[255,255\]' "$transcript"
grep -q '^overall=PASS$' "$transcript"
