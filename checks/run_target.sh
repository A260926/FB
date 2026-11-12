#!/bin/sh
set -eu
if ! test -s checks/test_patchmesh.cpp; then
    printf '%s\n' 'Missing required target source: checks/test_patchmesh.cpp' >&2
    exit 2
fi
mkdir -p .build
: "${CXX:=c++}"
"$CXX" -std=c++14 -O1 -DCATCH_CONFIG_NO_POSIX_SIGNALS -Iinclude -Idependencies/glm -Itests tests/test_main.cpp checks/test_patchmesh.cpp -o .build/patchmesh-tests
for name in 'patchmesh flat and curved' 'patchmesh seams and corners' 'patchmesh errors and limits'; do
    .build/patchmesh-tests "$name" --warn NoAssertions > .build/target-case.log 2>&1 || { cat .build/target-case.log; exit 1; }
    cat .build/target-case.log
    grep -q 'All tests passed (' .build/target-case.log
    grep -q 'in 1 test case)' .build/target-case.log
done
printf '%s\n' 'PATCHMESH 3 required C++ cases PASS'
