#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s tests/ducking_target.cpp; fi
mkdir -p .task-build
cmake -S . -B .task-build/core -DSHARED_LIBRARY=OFF -DBUILD_DEMOS=OFF -DWITH_C=OFF -DWITH_DOCS=OFF -DWITH_BINDING_DOCS=OFF -DWITH_PYTHON=OFF -DWITH_FFMPEG=OFF -DWITH_FFTW=OFF -DWITH_JACK=OFF -DWITH_LIBSNDFILE=OFF -DWITH_OPENAL=OFF -DWITH_RUBBERBAND=OFF -DWITH_SDL=OFF -DWITH_COREAUDIO=OFF -DWITH_PULSEAUDIO=OFF -DWITH_PIPEWIRE=OFF -DWITH_WASAPI=OFF > .task-build/configure.log 2>&1
cmake --build .task-build/core --clean-first --parallel 4 > .task-build/build.log 2>&1
cxx=${CXX:-c++}
lib=.task-build/core/libaudaspace.a
test -s "$lib"
"$cxx" -std=c++17 -Iinclude -I.task-build/core -Itask_support task_support/baseline.cpp "$lib" -pthread -o .task-build/baseline
if [[ $mode == target ]]; then
  test -s include/sequence/Ducking.h
test -s src/sequence/Ducking.cpp
test -s examples/ducking.cpp
# The mandatory source is always compiled, independent of CMake source listing.
"$cxx" -std=c++17 -Iinclude -I.task-build/core -Itask_support -include include/sequence/Ducking.h -c src/sequence/Ducking.cpp -o .task-build/ducking-bind.o
ar t "$lib" > .task-build/archive-members
if ! grep -q 'Ducking.cpp.o' .task-build/archive-members; then
  ar r "$lib" .task-build/ducking-bind.o
fi
"$cxx" -std=c++17 -Iinclude -I.task-build/core -Itask_support task_support/api_probe.cpp "$lib" -pthread -o .task-build/api-probe
.task-build/api-probe
"$cxx" -std=c++17 -I. -Iinclude -I.task-build/core tests/ducking_target.cpp task_support/driver.c "$lib" -pthread -o .task-build/target
"$cxx" -std=c++17 -Iinclude -I.task-build/core examples/ducking.cpp "$lib" -pthread -o .task-build/demo
"$cxx" -std=c++17 task_support/check_pcm.cpp -o .task-build/check-pcm
rm -f .task-build/ducking.f32
.task-build/demo .task-build/ducking.f32
test -s .task-build/ducking.f32
.task-build/check-pcm .task-build/ducking.f32
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in scene_clock envelope_seek entry_changes; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
