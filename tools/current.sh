#!/usr/bin/env bash
set -euo pipefail
build=$(mktemp -d 2>/dev/null || mktemp -d "${TMPDIR:-/tmp}/current.XXXXXX")
trap 'rm -r "$build"' EXIT
c++ -std=c++17 -O2 -Wall -Wextra -Iinclude src/camera.cpp src/image.cpp src/raster.cpp tests/current.cpp -o "$build/current"
"$build/current"
