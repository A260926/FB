#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
b=$(mktemp -d "${TMPDIR:-/tmp}/uf-target.XXXXXX")
trap 'rm -rf "$b"' EXIT

# The checks need the test-only instrumentation of the engine
# (DAN_TEST_INSTRUMENT) and the reference engine from checks/legacy_engine.cpp.
# The core source list is the one in checks/build.sh.
EXTRA_CXX_FLAGS=-DDAN_TEST_INSTRUMENT
EXTRA_INCLUDES=
source checks/build.sh

c++ -std=c++17 -O1 -DNDEBUG -DDAN_TEST_INSTRUMENT -pthread \
  -IDynamicAudioNormalizerAPI/include -IDynamicAudioNormalizerShared/include -IDynamicAudioNormalizerAPI/src -Ichecks \
  -c checks/legacy_engine.cpp -o "$b/legacy.o"

c++ -std=c++17 -O1 -DDAN_TEST_INSTRUMENT -pthread \
  -IDynamicAudioNormalizerAPI/include -IDynamicAudioNormalizerAPI/src -IDynamicAudioNormalizerShared/include -Ichecks \
  checks/target.cpp checks/suite_main.cpp "$b"/*.o -o "$b/target"

for group in equivalence window allocation; do
 "$b/target" "$group"
done
bash checks/baseline.sh
