#!/bin/sh
set -eu
mkdir -p .build
: "${CXX:=c++}"
"$CXX" -std=c++14 -O1 -Iinclude -Idependencies/glm checks/baseline.cpp -o .build/baseline
"$CXX" -std=c++14 -O1 -DCATCH_CONFIG_NO_POSIX_SIGNALS -Iinclude -Idependencies/glm -Itests tests/test_main.cpp tests/test_curve.cpp tests/test_rational_curve.cpp tests/test_surface.cpp tests/test_rational_surface.cpp -o .build/upstream-tests
printf '%s\n' 'BUILD baseline and upstream C++ tests PASS'
