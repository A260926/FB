#!/bin/sh
set -eu
mkdir -p .userfb-build
: "${CXX:=c++}"
"$CXX" -std=c++14 -O1 -Iinclude -Idependencies/glm userfb/baseline.cpp -o .userfb-build/baseline
"$CXX" -std=c++14 -O1 -DCATCH_CONFIG_NO_POSIX_SIGNALS -Iinclude -Idependencies/glm -Itests tests/test_main.cpp tests/test_curve.cpp tests/test_rational_curve.cpp tests/test_surface.cpp tests/test_rational_surface.cpp -o .userfb-build/upstream-tests
printf '%s\n' 'BUILD baseline and upstream C++ tests PASS'
