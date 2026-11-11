#!/usr/bin/env bash
set -euo pipefail
test -s mp4/defragment_recovery_test.go || { echo 'missing mp4/defragment_recovery_test.go' >&2; exit 2; }
log=$(mktemp)
trap 'rm -f "$log"' EXIT
bash support/go.sh test -json ./mp4 -run '^TestDefragmentRecovery(Groups|Overlap|Hybrid|Reject)$' -count=1 | tee "$log"
bash support/go.sh run ./support/check-tests "$log"
bash support/go.sh test ./mp4 -run '^TestDefragment(RoundTrip|Tracks|HybridFile|DefaultsAndFirstSampleFlags|ResolvesFullResend|ResolvesSupersededTail|ResolvesResetChain|ResolutionKeepsOtherTrackVerbatim|ResolutionShrinksGapPadding|FragmentSupersedesProgressiveSamples|AbandonedContentIsError)$' -count=1 -v
echo 'MP4FF_RECOVERY_TARGET_OK'
