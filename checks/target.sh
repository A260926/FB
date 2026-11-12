#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
b=$(mktemp -d "${TMPDIR:-/tmp}/uf-target.XXXXXX")
trap 'rm -rf "$b"' EXIT
source checks/build.sh
c++ -std=c++17 -O1 -pthread -IDynamicAudioNormalizerAPI/include -IDynamicAudioNormalizerAPI/src -IDynamicAudioNormalizerShared/include -Ichecks checks/target.cpp checks/suite_main.cpp "$b"/*.o -o "$b/target"
for group in equivalence window allocation; do
 "$b/target" "$group"
done
bash checks/baseline.sh
