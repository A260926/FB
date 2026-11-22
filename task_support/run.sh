#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s task_support/audio_render_test.cpp; fi
cmake -S . -B .task-build -DCMAKE_OSX_ARCHITECTURES="$(uname -m)" -DOTIO_PYTHON_INSTALL=OFF -DOTIO_AUTOMATIC_SUBMODULES=OFF -DOTIO_CXX_INSTALL=OFF -DOTIO_INSTALL_COMMANDLINE_TOOLS=OFF -DBUILD_TESTING=OFF -DIMATH_BUILD_TESTS=OFF -DIMATH_INSTALL=OFF -DOTIO_SHARED_LIBS=OFF -DMZ_ZLIB=OFF -DMZ_FETCH_LIBS=OFF -DCMAKE_DISABLE_FIND_PACKAGE_ZLIB=TRUE -DCMAKE_FIND_USE_PACKAGE_REGISTRY=FALSE -DCMAKE_FIND_USE_SYSTEM_PACKAGE_REGISTRY=FALSE
if [[ $mode != target ]]; then cmake --build .task-build --target task_baseline -j 4; fi
if [[ $mode == target ]]; then
  test -s src/opentimelineio/audioRender.h
test -s src/opentimelineio/audioRender.cpp
test -s src/opentimelineio/pcmWave.h
test -s src/opentimelineio/pcmWave.cpp
test -s examples/audio_render.cpp
cmake -E rm -f .task-build/src/opentimelineio/CMakeFiles/opentimelineio.dir/audioRender.cpp.o .task-build/src/opentimelineio/CMakeFiles/opentimelineio.dir/pcmWave.cpp.o .task-build/task_support/CMakeFiles/task_target.dir/audio_render_test.cpp.o .task-build/task_support/CMakeFiles/audio_render.dir/__/examples/audio_render.cpp.o
cmake --build .task-build --target task_target audio_render task_fixture -j 4
test -x .task-build/audio_render
# Regression cases run the actual CLI with their independently generated fixtures.
export TASK_AUDIO_RENDER="$PWD/.task-build/audio_render"
.task-build/task_fixture make .task-build
rm -f .task-build/example-output.wav
.task-build/audio_render .task-build/example-input.otio .task-build .task-build/example-output.wav 48000 3
test -s .task-build/example-output.wav
.task-build/task_fixture check .task-build/example-output.wav
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in coordinates dissolve streaming; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
if [[ $mode != target ]]; then .task-build/task_support/task_baseline; fi
