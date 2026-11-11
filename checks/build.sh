#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p .author-build
cxx=${CXX:-c++}
flags=(-std=c++11 -O2 -DSOUNDTOUCH_FLOAT_SAMPLES -DSOUNDTOUCH_DISABLE_X86_OPTIMIZATIONS -Iinclude -Isource/SoundTouch)
sources=(AAFilter FIRFilter FIFOSampleBuffer RateTransposer SoundTouch TDStretch cpu_detect_x86 BPMDetect PeakFinder InterpolateLinear InterpolateCubic InterpolateShannon)
objects=()
for name in "${sources[@]}"; do
  "$cxx" "${flags[@]}" -c "source/SoundTouch/$name.cpp" -o ".author-build/$name.o"
  objects+=(".author-build/$name.o")
done
ar rcs .author-build/libSoundTouch.a "${objects[@]}"
"$cxx" "${flags[@]}" source/SoundStretch/main.cpp source/SoundStretch/RunParameters.cpp source/SoundStretch/WavFile.cpp .author-build/libSoundTouch.a -o .author-build/soundstretch
"$cxx" "${flags[@]}" checks/baseline.cpp .author-build/libSoundTouch.a -o .author-build/baseline
printf 'BUILD_OK float scalar c++11\n'
