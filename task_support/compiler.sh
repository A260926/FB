#!/bin/bash
set -euo pipefail
# The upstream vector kernels target x86. macOS arm64 uses its installed x86_64 toolchain.
base_flags=(-std=c11 -O1 -fno-fast-math -I. -Isrc)
if [[ $(uname -s) == Darwin && $(uname -m) == arm64 ]]; then
  base_flags+=(-arch x86_64)
fi
exec cc "${base_flags[@]}" "$@"
