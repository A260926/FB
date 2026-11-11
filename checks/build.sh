#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p .build
cxx=${CXX:-c++}
flags=(-std=c++11 -O2 -DSOUNDTOUCH_FLOAT_SAMPLES -DSOUNDTOUCH_DISABLE_X86_OPTIMIZATIONS -Iinclude -Isource/SoundTouch)
sources=(AAFilter FIRFilter FIFOSampleBuffer RateTransposer SoundTouch TDStretch cpu_detect_x86 BPMDetect PeakFinder InterpolateLinear InterpolateCubic InterpolateShannon)
objects=()
for name in "${sources[@]}"; do
  "$cxx" "${flags[@]}" -c "source/SoundTouch/$name.cpp" -o ".build/$name.o"
  objects+=(".build/$name.o")
done
ar rcs .build/libSoundTouch.a "${objects[@]}"
"$cxx" "${flags[@]}" source/SoundStretch/main.cpp source/SoundStretch/RunParameters.cpp source/SoundStretch/WavFile.cpp .build/libSoundTouch.a -o .build/soundstretch
"$cxx" "${flags[@]}" checks/baseline.cpp .build/libSoundTouch.a -o .build/baseline
printf 'BUILD_OK float scalar c++11\n'
