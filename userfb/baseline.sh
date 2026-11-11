#!/bin/sh
set -eu
b=$(mktemp -d)
trap 'rm -rf "$b"' EXIT HUP INT TERM
c++ -std=c++14 -O1 -pthread -I Cpp/Source userfb/baseline.cpp Cpp/Source/Test.cpp Cpp/Source/Maths.cpp Cpp/Source/enkiTS/TaskScheduler.cpp Cpp/Source/enkiTS/TaskScheduler_c.cpp -o "$b/baseline"
"$b/baseline"
