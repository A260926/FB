#!/bin/bash
cxx=${CXX:-c++}
flags=(-std=c++17 -O0 -DNDEBUG -DJUCE_GLOBAL_MODULE_SETTINGS_INCLUDED=1 -DJUCE_STANDALONE_APPLICATION=1 -DJUCE_USE_CURL=0 -DJUCE_WEB_BROWSER=0 -I. -Imodules -pthread)
if [[ $(uname -s) == Darwin ]]; then
 extension=mm
 platform=(-framework Cocoa -framework Foundation -framework IOKit -framework Security -framework Accelerate)
else
 extension=cpp
 platform=(-ldl -lrt)
fi
objects=(.task-build/juce_core.o .task-build/juce_audio_basics.o)
