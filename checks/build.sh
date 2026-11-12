#!/usr/bin/env bash
set -euo pipefail
# Compiles the API core sources used by the checks.
# Extra flags can be passed in via EXTRA_CXX_FLAGS / EXTRA_INCLUDES.
EXTRA_CXX_FLAGS=${EXTRA_CXX_FLAGS:-}
EXTRA_INCLUDES=${EXTRA_INCLUDES:-}
for name in DynamicAudioNormalizer FrameBuffer GaussianFilter Logging Version Binding_C; do
 c++ -std=c++17 -O1 -DNDEBUG ${EXTRA_CXX_FLAGS} -pthread ${EXTRA_INCLUDES} -IDynamicAudioNormalizerAPI/include -IDynamicAudioNormalizerShared/include -c "DynamicAudioNormalizerAPI/src/$name.cpp" -o "$b/$name.o"
done
