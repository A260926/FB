#!/bin/bash
set -euo pipefail
build=$1
cmake -S . -B "$build" -DBUILD_PROG=OFF -DBUILD_SHARED_LIBS=OFF -DCMAKE_BUILD_TYPE=Release -DENABLE_ZLIB=OFF -DENABLE_PNG=OFF -DENABLE_GIF=OFF -DENABLE_JPEG=OFF -DENABLE_TIFF=OFF -DENABLE_WEBP=OFF -DENABLE_OPENJPEG=OFF > "$build/config.log" 2>&1 || { cat "$build/config.log"; exit 1; }
cmake --build "$build" --target leptonica -j4 > "$build/build.log" 2>&1 || { cat "$build/build.log"; exit 1; }
cat "$build/config.log" "$build/build.log"
