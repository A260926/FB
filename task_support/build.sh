#!/bin/bash
set -euo pipefail
source task_support/flags.sh
mkdir -p .task-build
rm -f .task-build/juce_core.o .task-build/juce_audio_basics.o .task-build/baseline .task-build/api-probe .task-build/target .task-build/atomic-render
for module in juce_core juce_audio_basics; do
 "$cxx" "${flags[@]}" -c "modules/$module/$module.$extension" -o ".task-build/$module.o"
done
"$cxx" "${flags[@]}" task_support/baseline.cpp "${objects[@]}" "${platform[@]}" -o .task-build/baseline
