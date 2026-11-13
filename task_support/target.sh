#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
bash task_support/build.sh
# Compile the exact new test path declared in the public request. There is no new shell runner to trust.
"${CXX:-c++}" -std=c++17 -O2 -DSOUNDTOUCH_DISABLE_X86_OPTIMIZATIONS -Iinclude -Isource/SoundTouch -I.build -Itask_support task_support/test_main.cpp tests/time_anchor_test.cpp .build/libsoundtouch.a -o .build/time-anchor-tests
set +e
.build/time-anchor-tests > .build/time-anchor.stdout 2> .build/time-anchor.stderr
status=$?
set -e
printf 'ANCHOR_PROCESS_EXIT status=%s\n' "$status"
cat .build/time-anchor.stdout
cat .build/time-anchor.stderr >&2
if [ "$status" -ne 0 ]; then exit "$status"; fi
for group in islands stretch stream; do
  awk -v g="$group" '$1=="ANCHOR_GROUP" && $2==g && $3 ~ /^tests=[1-9][0-9]*$/ && $4 ~ /^checks=[1-9][0-9]*$/ && $5=="DONE" {n++} END {exit n!=1}' .build/time-anchor.stdout
done
test "$(awk '$0=="ANCHOR_SUITE_DONE groups=3"{n++}END{print n+0}' .build/time-anchor.stdout)" = 1
bash task_support/baseline.sh
"${CXX:-c++}" -std=c++17 -O2 -DSOUNDTOUCH_DISABLE_X86_OPTIMIZATIONS -Iinclude -Isource/SoundTouch -I.build examples/time_anchor.cpp .build/libsoundtouch.a -o .build/time-anchor-example
rm -f .build/time-anchor-example.wav
.build/time-anchor-example
test -f .build/time-anchor-example.wav
test "$(wc -c < .build/time-anchor-example.wav)" -gt 44
test "$(dd if=.build/time-anchor-example.wav bs=1 count=4 2>/dev/null)" = RIFF
printf 'ANCHOR_EXAMPLE_WAV_OK\n'
