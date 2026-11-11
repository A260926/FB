#!/bin/sh
set -eu
b=$(mktemp -d 2>/dev/null || mktemp -d "${TMPDIR:-/tmp}/baseline.XXXXXX")
trap 'rm -rf "$b"' EXIT HUP INT TERM
c++ -std=c++14 -O1 -pthread -I Cpp/Source sanity/baseline.cpp Cpp/Source/Test.cpp Cpp/Source/Maths.cpp Cpp/Source/enkiTS/TaskScheduler.cpp Cpp/Source/enkiTS/TaskScheduler_c.cpp -o "$b/baseline"
"$b/baseline"
