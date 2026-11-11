#!/usr/bin/env bash
set -euo pipefail
test -s tests/wav_cue_labels_test.c || { echo 'missing tests/wav_cue_labels_test.c'; exit 1; }
build=$(mktemp -d)
trap 'rm -rf "$build"' EXIT
cmake -S . -B "$build" -DBUILD_SHARED_LIBS=OFF -DBUILD_TESTING=ON -DBUILD_PROGRAMS=OFF -DBUILD_EXAMPLES=OFF -DENABLE_EXTERNAL_LIBS=OFF -DENABLE_MPEG=OFF
cmake --build "$build" --target wav_cue_labels_test command_test --parallel 4
ctest --test-dir "$build" -R '^wav_cue_labels$' -V --output-on-failure > "$build/target.log" 2>&1
cat "$build/target.log"
grep -Eq '1/1 Test.*wav_cue_labels.*Passed' "$build/target.log"
grep -q 'WAV_CUE_LABELS_OK' "$build/target.log"
mkdir -p "$build/old-cue"
cd "$build/old-cue"
"$build/command_test" cue
echo SNDFILE_OLD_CUE_PASS
