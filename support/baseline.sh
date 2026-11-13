#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
args=()
while IFS= read -r option; do args+=("-D${option}=OFF"); done < <(sed -nE 's/^[[:space:]]*option\((MOD_[A-Z0-9_]+|SWIG_[A-Z0-9_]+).*/\1/p' CMakeLists.txt)
cmake -S . -B .build "${args[@]}" -DBUILD_TESTING=OFF -DBUILD_DOCS=OFF -DRELOCATABLE=OFF
cmake --build .build --target baseline -j 4
MLT_DATA="$PWD/profiles" .build/out/bin/baseline
