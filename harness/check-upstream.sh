#!/bin/bash
set -euo pipefail
b=$(mktemp -d)
trap 'rm -rf "$b"' EXIT
c++ -std=c++11 -O1 -I. QuickHull.cpp Tests/main.cpp Tests/QuickHullTests.cpp -o "$b/tests"
"$b/tests" >"$b/result" 2>&1
cat "$b/result"
! grep -qi 'test failed' "$b/result"
grep -Fx 'QuickHull tests succesfully passed.' "$b/result"
echo QUICKHULL_UPSTREAM_OK
