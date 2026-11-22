#!/bin/bash
set -euo pipefail
mode=${1:-baseline}
extra_mode=OFF
if [[ $mode == target ]]; then
  extra_mode=ON
  # Rebuild required additions even for edits within one Make timestamp tick.
  rm -f .task-build/build/CMakeFiles/OpenAL.dir/core/effectslottail.cpp.o \
    .task-build/build/CMakeFiles/task_target.dir/tests/task_slot_tail.cpp.o \
    .task-build/build/CMakeFiles/task_target.dir/required_tail_header.cpp.o \
    .task-build/build/CMakeFiles/task_example.dir/examples/slot_tail_render.cpp.o
fi
cmake -S . -B .task-build/build -DCMAKE_BUILD_TYPE=Release -DLIBTYPE=SHARED -DTASK_TARGET_MODE="$extra_mode" \
 -DALSOFT_ENABLE_MODULES=OFF -DALSOFT_EXAMPLES=OFF -DALSOFT_UTILS=OFF -DALSOFT_TESTS=OFF \
 -DALSOFT_EMBED_HRTF_DATA=OFF -DALSOFT_INSTALL=OFF -DALSOFT_UPDATE_BUILD_VERSION=OFF \
 -DALSOFT_RTKIT=OFF -DALSOFT_DLOPEN=OFF -DALSOFT_BACKEND_COREAUDIO=OFF -DALSOFT_BACKEND_WAVE=OFF \
 -DALSOFT_BACKEND_PIPEWIRE=OFF -DALSOFT_BACKEND_PULSEAUDIO=OFF -DALSOFT_BACKEND_ALSA=OFF \
 -DALSOFT_BACKEND_OSS=OFF -DALSOFT_BACKEND_SOLARIS=OFF -DALSOFT_BACKEND_SNDIO=OFF \
 -DALSOFT_BACKEND_WASAPI=OFF -DALSOFT_BACKEND_DSOUND=OFF -DALSOFT_BACKEND_WINMM=OFF \
 -DALSOFT_BACKEND_JACK=OFF -DALSOFT_BACKEND_OBOE=OFF -DALSOFT_BACKEND_OPENSL=OFF \
 -DALSOFT_BACKEND_PORTAUDIO=OFF -DALSOFT_BACKEND_SDL2=OFF -DALSOFT_BACKEND_SDL3=OFF
cmake --build .task-build/build --target task_baseline -j 4
