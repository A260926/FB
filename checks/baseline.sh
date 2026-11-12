#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
b=$(mktemp -d "${TMPDIR:-/tmp}/dspatch-base.XXXXXX")
trap 'rm -rf "$b"' EXIT
c++ -std=c++14 -O1 -pthread -Iinclude -Isrc checks/baseline.cpp src/{Circuit,Component,Signal,SignalBus}.cpp src/internal/*.cpp -o "$b/base"
"$b/base"
c++ -std=c++14 -O0 -pthread -DCATCH_CONFIG_NO_POSIX_SIGNALS -Iinclude -Isrc -Itests -Isubprojects/catch tests/main.cpp src/{Circuit,Component,Signal,SignalBus}.cpp src/internal/*.cpp -o "$b/old"
"$b/old" 'SerialTest,FeedbackTest,NoOutputTest'
