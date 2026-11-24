#!/bin/bash
set -euo pipefail
cmake -S . -B .task-build/cmake -DlibADLMIDI_STATIC=ON -DlibADLMIDI_SHARED=OFF -DUSE_DOSBOX_EMULATOR=ON -DUSE_NUKED_EMULATOR=OFF -DUSE_OPAL_EMULATOR=OFF -DUSE_JAVA_EMULATOR=OFF -DUSE_ESFMU_EMULATOR=OFF -DUSE_MAME_EMULATOR=OFF -DUSE_YMFM_EMULATOR=OFF -DUSE_NUKED_OPL2_LLE_EMULATOR=OFF -DUSE_NUKED_OPL3_LLE_EMULATOR=OFF -DWITH_MIDI_SEQUENCER=ON -DWITH_EMBEDDED_BANKS=ON -DWITH_HQ_RESAMPLER=OFF -DWITH_XMI_SUPPORT=OFF -DWITH_GENADLDATA=OFF -DWITH_GENADLDATA_RUN=OFF -DWITH_UNIT_TESTS=OFF -DWITH_MIDIPLAY=OFF -DWITH_ADLMIDI2=OFF -DWITH_OLD_UTILS=OFF -DWITH_VLC_PLUGIN=OFF -DEXAMPLE_SDL2_AUDIO=OFF -DUSE_HW_SERIAL=OFF -DCMAKE_BUILD_TYPE=Release
cmake --build .task-build/cmake --target ADLMIDI_static --clean-first --parallel 4
c++ -std=c++11 -O2 -Iinclude task_support/baseline.cpp .task-build/cmake/libADLMIDI.a -lm -o .task-build/baseline
if [[ ${1:-baseline} == target ]]; then
  test -s src/adlmidi_smf2.hpp
  test -s src/adlmidi_smf2.cpp
  ar -t .task-build/cmake/libADLMIDI.a | grep -Fx 'adlmidi_smf2.cpp.o'
fi
