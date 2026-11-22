#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s tests/reset_state_test.c; fi
bash task_support/build.sh
if [[ $mode == target ]]; then
  test -s doc/examples/reset_roundtrip.c
cc -std=c99 task_support/check_wave.c -o .task-build/check-wave
.task-build/float/reset-probe fixture
for numeric in float fixed; do
  cc -std=c99 -I. -Iinclude -Itask_support/include tests/reset_state_test.c task_support/driver.c ".task-build/$numeric/libspeex.a" -lm -o ".task-build/target-$numeric"
  cc -std=c99 -Iinclude -Itask_support/include -c doc/examples/reset_roundtrip.c -o ".task-build/roundtrip-$numeric.o"
  for symbol in speex_encode_int speex_decode_int speex_encoder_ctl speex_decoder_ctl; do
    nm -u ".task-build/roundtrip-$numeric.o" | grep -Eq "_?${symbol}$"
  done
  cc ".task-build/roundtrip-$numeric.o" ".task-build/$numeric/libspeex.a" -lm -o ".task-build/reset-roundtrip-$numeric"
  rm -f ".task-build/output-$numeric.wav"
  ".task-build/reset-roundtrip-$numeric" .task-build/input.wav ".task-build/output-$numeric.wav" 3
  test -s ".task-build/output-$numeric.wav"
  .task-build/check-wave ".task-build/output-$numeric.wav"
done
cp task_support/run_variants.sh .task-build/target
chmod +x .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in encoder decoder recursive; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
if [[ $mode == target ]]; then
  .task-build/float/reset-probe normal
  .task-build/fixed/reset-probe normal
else
  .task-build/float/reset-probe repro
  .task-build/fixed/reset-probe repro
fi
