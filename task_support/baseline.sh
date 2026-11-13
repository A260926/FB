#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p .build/old-input
.build/baseline
.build/soundstretch .build/input.wav .build/output.wav -tempo=25 > .build/soundstretch.stdout 2> .build/soundstretch.stderr
# Check RIFF header and an actual nonempty output, not merely the CLI exit code.
bytes=$(wc -c < .build/output.wav)
test "$bytes" -gt 100000
test "$(dd if=.build/output.wav bs=1 count=4 2>/dev/null)" = RIFF
printf 'SOUNDSTRETCH_WAV_OK tempo_percent=25\n'
# Retain all eight upstream run_test invocations, substituting only private paths.
rm -rf .build/upstream-test-out
sed -e 's|^SOUND_DIR=.*|SOUND_DIR=.build/old-input|' -e 's|^OUT_DIR=.*|OUT_DIR=.build/upstream-test-out|' -e 's|^SS=.*|SS=.build/soundstretch|' bin/run_test > .build/upstream-run-test
bash -e .build/upstream-run-test > .build/upstream-run-test.stdout 2> .build/upstream-run-test.stderr
count=0
for bits in 8 16 24 32; do
  for channels in 1 2; do
    file=.build/upstream-test-out/out-${bits}b${channels}.wav
    test "$(wc -c < "$file")" -gt 1000
    test "$(dd if="$file" bs=1 count=4 2>/dev/null)" = RIFF
    count=$((count+1))
  done
done
printf 'UPSTREAM_RUN_TEST_OK files=%s pitch=-3 bpm=1\n' "$count"
