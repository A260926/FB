#!/bin/bash
set -euo pipefail
mkdir -p .task-build
if [[ ! -f codec/common/inc/version_gen.h ]]; then
  sed 's/\$FULL_VERSION/"1bca623"/g' codec/common/inc/version_gen.h.template > codec/common/inc/version_gen.h
fi
make -j4 libopenh264.a USE_ASM=No V=No -o codec/common/inc/version_gen.h
