#!/usr/bin/env bash
# Builds the library from source, compiles and runs the rate ramp test and
# example, then re-runs the legacy baseline (constant rate, anti-alias and
# SoundStretch file processing).
set -euo pipefail
cd "$(dirname "$0")/.."

bash checks/build.sh

cxx=${CXX:-c++}
flags=(-std=c++11 -O2 -DSOUNDTOUCH_FLOAT_SAMPLES -DSOUNDTOUCH_DISABLE_X86_OPTIMIZATIONS -Iinclude -Isource/SoundTouch)

"$cxx" "${flags[@]}" checks/rate_ramp_test.cpp .build/libSoundTouch.a -o .build/rate_ramp_test
"$cxx" "${flags[@]}" examples/rate_ramp.cpp .build/libSoundTouch.a -o .build/rate_ramp_example

.build/rate_ramp_test
.build/rate_ramp_example .build/rate-ramp.csv
test -s .build/rate-ramp.csv

bash checks/run_baseline.sh

printf 'RATE_RAMP_SCRIPT_OK\n'
