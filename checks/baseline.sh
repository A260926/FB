#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
b=$(mktemp -d "${TMPDIR:-/tmp}/dan-base.XXXXXX")
trap 'rm -rf "$b"' EXIT
source checks/build.sh
c++ -std=c++17 -O1 -pthread -IDynamicAudioNormalizerAPI/include checks/baseline.cpp "$b"/*.o -o "$b/base"
"$b/base"
