#!/bin/sh
set -eu
if ! test -s userfb/test_patchmesh.cpp; then
    printf '%s\n' 'Missing required target source: userfb/test_patchmesh.cpp' >&2
    exit 2
fi
mkdir -p .userfb-build
: "${CXX:=c++}"
"$CXX" -std=c++14 -O1 -DCATCH_CONFIG_NO_POSIX_SIGNALS -Iinclude -Idependencies/glm -Itests tests/test_main.cpp userfb/test_patchmesh.cpp -o .userfb-build/patchmesh-tests
for name in 'patchmesh flat and curved' 'patchmesh seams and corners' 'patchmesh errors and limits'; do
    .userfb-build/patchmesh-tests "$name" --warn NoAssertions > .userfb-build/target-case.log 2>&1 || { cat .userfb-build/target-case.log; exit 1; }
    cat .userfb-build/target-case.log
    grep -q 'All tests passed (' .userfb-build/target-case.log
    grep -q 'in 1 test case)' .userfb-build/target-case.log
done
printf '%s\n' 'PATCHMESH 3 required C++ cases PASS'
