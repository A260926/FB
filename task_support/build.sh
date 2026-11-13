#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p .build/obj
printf '#define SOUNDTOUCH_FLOAT_SAMPLES 1\n#define SOUNDTOUCH_DISABLE_X86_OPTIMIZATIONS 1\n' > .build/soundtouch_config.h
cxx=${CXX:-c++}
flags=(-std=c++17 -O2 -DSOUNDTOUCH_DISABLE_X86_OPTIMIZATIONS -Iinclude -Isource/SoundTouch -I.build -Itask_support)
objects=()
for file in source/SoundTouch/*.cpp; do
  object=.build/obj/$(basename "${file%.cpp}").o
  "$cxx" "${flags[@]}" -c "$file" -o "$object"
  objects+=("$object")
done
ar rcs .build/libsoundtouch.a "${objects[@]}"
"$cxx" "${flags[@]}" task_support/baseline.cpp .build/libsoundtouch.a -o .build/baseline
"$cxx" "${flags[@]}" source/SoundStretch/{main,RunParameters,WavFile}.cpp .build/libsoundtouch.a -o .build/soundstretch
printf 'BUILD_OK float=1 scalar=1 cxx17=1\n'
