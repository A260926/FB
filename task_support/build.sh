#!/bin/bash
set -euo pipefail
mkdir -p .task-build
if [[ ! -s .task-build/Makefile ]]; then
  (cd .task-build && ../configure --target=generic-gnu --disable-vp8 --enable-vp9 --disable-vp9-highbitdepth --disable-runtime-cpu-detect --disable-examples --disable-tools --disable-unit-tests --disable-docs --disable-webm-io --disable-libyuv --disable-shared --enable-static)
fi
# Each outer goal recursively builds both MAKECMDGOALS; invoke them separately.
make -C .task-build -j4 libvpx.a
make -C .task-build -j4 libvpxrc.a
