#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s test/audio_tap.c; fi
platform_libs="-ldl -lrt"
if [[ $(uname -s) == Darwin ]]; then platform_libs="-framework CoreAudio -framework AudioToolbox -framework CoreFoundation -framework Foundation -framework CoreVideo -framework IOKit -framework Cocoa -liconv -lobjc -framework UserNotifications -framework Security -framework UniformTypeIdentifiers"; fi
mkdir -p .task-build
cmake -S . -B .task-build/lib -DSDL_SHARED=OFF -DSDL_STATIC=ON -DSDL_TESTS=OFF -DSDL_TEST_LIBRARY=OFF -DSDL_EXAMPLES=OFF -DSDL_VIDEO=OFF -DSDL_GPU=OFF -DSDL_RENDER=OFF -DSDL_CAMERA=OFF -DSDL_JOYSTICK=OFF -DSDL_HAPTIC=OFF -DSDL_HIDAPI=OFF -DSDL_POWER=OFF -DSDL_SENSOR=OFF -DSDL_DIALOG=OFF -DSDL_TRAY=OFF -DSDL_NOTIFICATION=OFF -DSDL_ALSA=OFF -DSDL_JACK=OFF -DSDL_PULSEAUDIO=OFF -DSDL_SNDIO=OFF -DSDL_OSS=OFF -DSDL_LIBUDEV=OFF -DSDL_RPATH=OFF
cmake --build .task-build/lib -j 4
if [[ $mode == target ]]; then
  test -s test/audio_tap.c
test -s examples/audio/tap.c
cc -std=c11 -Iinclude -I.task-build/lib/include-config-release -I. task_support/api_consumer.c .task-build/lib/libSDL3.a -lm -pthread $platform_libs -o .task-build/api-consumer
.task-build/api-consumer
cc -std=c11 -Iinclude -I.task-build/lib/include-config-release -I. examples/audio/tap.c .task-build/lib/libSDL3.a -lm -pthread $platform_libs -o .task-build/tap-demo
.task-build/tap-demo > .task-build/demo.log
cat .task-build/demo.log
grep -qx AUDIO_TAP_DEMO_OK .task-build/demo.log
cc -std=c11 -Iinclude -I.task-build/lib/include-config-release -I. task_support/driver.c test/audio_tap.c .task-build/lib/libSDL3.a -lm -pthread $platform_libs -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in fanout ownership conversion; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
cc -std=c11 -Iinclude -I.task-build/lib/include-config-release -I. task_support/baseline.c .task-build/lib/libSDL3.a -lm -pthread $platform_libs -o .task-build/baseline
.task-build/baseline
