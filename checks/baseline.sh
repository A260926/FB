#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
mkdir -p .build
c++ -std=c++11 -O0 -Isrc checks/probe.cpp -o .build/probe
.build/probe
c++ -std=c++11 -O0 test/test.cpp -o .build/upstream-test
.build/upstream-test --plain --subset save_restore
.build/upstream-test --plain --subset clip\*
.build/upstream-test --plain --subset global_composite_operation
