#!/bin/bash
set -euo pipefail
b=$(mktemp -d)
trap 'rm -rf "$b"' EXIT
c++ -std=c++14 -O1 -g -fsanitize=address -fno-omit-frame-pointer -Iinclude checks/repro.cc -o "$b/repro"
ASAN_OPTIONS=detect_leaks=0 "$b/repro" "$@"
