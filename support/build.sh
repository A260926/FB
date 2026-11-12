#!/bin/bash
set -euo pipefail
build=$1
mkdir -p "$build"
cmake -S . -B "$build/lib" -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF -DWEBP_BUILD_ANIM_UTILS=OFF -DWEBP_BUILD_CWEBP=OFF -DWEBP_BUILD_DWEBP=OFF -DWEBP_BUILD_GIF2WEBP=OFF -DWEBP_BUILD_IMG2WEBP=OFF -DWEBP_BUILD_VWEBP=OFF -DWEBP_BUILD_WEBPINFO=OFF -DWEBP_BUILD_WEBPMUX=OFF -DWEBP_BUILD_LIBWEBPMUX=OFF -DWEBP_BUILD_EXTRAS=OFF -DWEBP_BUILD_FUZZTEST=OFF -DWEBP_BUILD_WEBP_JS=OFF -DWEBP_FIND_IMG_LIBS=OFF -DCMAKE_DISABLE_FIND_PACKAGE_OpenGL=ON -DWEBP_USE_THREAD=ON -DFETCHCONTENT_FULLY_DISCONNECTED=ON
cmake --build "$build/lib" --target webp --parallel 4
if [[ ${2:-baseline} == target ]]; then src=(support/target_main.c tests/color_contract.c); else src=(support/baseline.c support/alpha_order.c); fi
cc -O2 -std=c99 -I. -Isupport "${src[@]}" "$build/lib/libwebp.a" "$build/lib/libsharpyuv.a" -lm -pthread -o "$build/run"
