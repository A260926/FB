#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
cmake -S . -B .build -DOTIO_PYTHON_INSTALL=OFF -DOTIO_AUTOMATIC_SUBMODULES=OFF -DOTIO_CXX_INSTALL=OFF -DOTIO_INSTALL_COMMANDLINE_TOOLS=OFF -DBUILD_TESTING=OFF -DIMATH_BUILD_TESTS=OFF -DIMATH_INSTALL=OFF -DOTIO_SHARED_LIBS=OFF -DMZ_ZLIB=OFF
cmake --build .build --target baseline -j 4
.build/support/baseline
