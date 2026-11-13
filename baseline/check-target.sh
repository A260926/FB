#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "$0")/.."
for f in extras/record_reader.hpp extras/record_reader.cpp tests/test_record_reader.cpp; do
  test -s "$f" || { echo "missing target file: $f" >&2; exit 1; }
done
bash baseline/run.sh
"${CXX:-c++}" -std=c++17 -O1 -g -Wall -Wextra -fsanitize=address,undefined -fno-omit-frame-pointer src/pugixml.cpp extras/record_reader.cpp tests/test.cpp tests/main.cpp tests/test_parse.cpp tests/test_document.cpp tests/test_dom_traverse.cpp tests/test_record_reader.cpp -o .baseline-build/target
if [[ "$(uname -s)" == Darwin ]]; then export ASAN_OPTIONS=detect_leaks=0; else export ASAN_OPTIONS=detect_leaks=1; fi
.baseline-build/target
