#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
b=$(mktemp -d "${TMPDIR:-/tmp}/wdf-base.XXXXXX")
trap 'rm -rf "$b"' EXIT
c++ -std=c++17 -O1 -Iinclude checks/baseline.cpp -o "$b/base"
"$b/base"
c++ -std=c++17 -O0 -DCATCH_CONFIG_NO_POSIX_SIGNALS -Iinclude -Itests tests/TestRunner.cpp tests/StaticBasicCircuitTest.cpp -o "$b/old"
"$b/old" 'Static Basic Circuits Test'
