#!/usr/bin/env bash
set -euo pipefail
for name in DynamicAudioNormalizer FrameBuffer GaussianFilter Logging Version Binding_C; do
 c++ -std=c++17 -O1 -DNDEBUG -pthread -IDynamicAudioNormalizerAPI/include -IDynamicAudioNormalizerShared/include -c "DynamicAudioNormalizerAPI/src/$name.cpp" -o "$b/$name.o"
done
