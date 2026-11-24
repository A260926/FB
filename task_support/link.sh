#!/bin/bash
set -euo pipefail
lib=.task-build/cmake/libADLMIDI.a
case $(uname -s) in
  Darwin) lib_args=("-Wl,-force_load,$lib") ;;
  *) lib_args=(-Wl,--whole-archive "$lib" -Wl,--no-whole-archive) ;;
esac
c++ -std=c++11 -O2 -I. -Iinclude -include src/adlmidi_smf2.hpp -x c++ task_support/driver.c tests/smf2_layered.cpp -x none "${lib_args[@]}" -lm -o .task-build/target
c++ -std=c++11 -O2 -Iinclude -include src/adlmidi_smf2.hpp task_support/api_consumer.cpp "${lib_args[@]}" -lm -o .task-build/api-consumer
.task-build/api-consumer
test -s examples/smf2_layered.cpp
c++ -std=c++11 -O2 -Iinclude -include src/adlmidi_smf2.hpp examples/smf2_layered.cpp "${lib_args[@]}" -lm -o .task-build/smf2-example
rm -f .task-build/smf2-example.pcm
.task-build/smf2-example .task-build/smf2-example.pcm
test -s .task-build/smf2-example.pcm
test "$(wc -c < .task-build/smf2-example.pcm)" -ge 1024
.task-build/pcm-check .task-build/smf2-example.pcm
