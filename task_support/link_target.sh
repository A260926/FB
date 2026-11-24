#!/bin/bash
set -euo pipefail
source task_support/flags.sh
header=modules/juce_audio_basics/sources/juce_BufferedAudioTimeline.h
production=modules/juce_audio_basics/sources/juce_BufferedAudioTimeline.cpp
test -s "$header"
test -s "$production"
test -s examples/atomic_timeline_render.cpp
grep -Fq '#include "sources/juce_BufferedAudioTimeline.h"' modules/juce_audio_basics/juce_audio_basics.h
grep -Fq '#include "sources/juce_BufferedAudioTimeline.cpp"' modules/juce_audio_basics/juce_audio_basics.cpp
"$cxx" "${flags[@]}" task_support/api_probe.cpp "${objects[@]}" "${platform[@]}" -o .task-build/api-probe
.task-build/api-probe
"$cxx" "${flags[@]}" examples/atomic_timeline_render.cpp "${objects[@]}" "${platform[@]}" -o .task-build/atomic-render
rm -f .task-build/atomic-mix.f32
.task-build/atomic-render .task-build
test -s .task-build/atomic-mix.f32
test "$(wc -c < .task-build/atomic-mix.f32)" -eq 2048
"$cxx" "${flags[@]}" -x c++ task_support/driver.c task_support/atomic_timeline_test.cpp -x none "${objects[@]}" "${platform[@]}" -o .task-build/target
